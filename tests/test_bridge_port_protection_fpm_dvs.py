"""TCP FPM-to-real-fpmsyncd integration.

Synthetic peer uses the shared-header reference encoder where stated; it does not claim
that the companion zebra dispatcher/getters have been integrated.
"""
import struct
import pytest
from dvslib.dvs_bridge_port_protection import (
    bpp, bpp_instrumented, bpp_fpm, FpmSession, BINDING, OBJECTS, FPM_IDS, CONFIG, STATE,
    BP, NHG, NH, TUNNEL, GROUP_ATTR, ADMIN_ATTR, run, poll, stable,
    frame, binding_frame, member_frame, group_frame, restart_fpmsyncd_after_sigkill,
)


def exact(bpp, table, key, expected):
    bpp.app_db.wait_for_field_match(table,key,expected)
    assert bpp.app_db.get_entry(table,key)==expected


def member_row(ip, mac='02:00:00:00:00:02', kind='l3'):
    return {'remote_vtep':ip,'type':kind,'router_mac':mac if kind=='l3' else ''}


def assert_fpm_path(bpp,expected):
    state=bpp.state()
    assert state['protection_state']==expected
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[ADMIN_ATTR]=='SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_'+expected
    assert state['reconciliation_state']=='STEADY'


@pytest.mark.parametrize('identifier',[0x80000000,0x80000001,0x8fffffff])
@pytest.mark.parametrize('producer',['crafted','reference_cpp'])
def test_binding_uint32_add_update_duplicate_and_delete(bpp,bpp_fpm,identifier,producer):
    index=bpp_fpm.ifindex()
    data=(bpp_fpm.encode('binding',index,'add',identifier) if producer=='reference_cpp'
          else binding_frame(index,identifier))
    bpp_fpm.send(data)
    exact(bpp,BINDING,bpp.po,{'nexthop_group':str(identifier)})
    bpp_fpm.send(data)
    exact(bpp,BINDING,bpp.po,{'nexthop_group':str(identifier)})
    bpp_fpm.send(binding_frame(index,bpp.group_id))
    exact(bpp,BINDING,bpp.po,{'nexthop_group':str(bpp.group_id)})
    data=(bpp_fpm.encode('binding',index,'del',0) if producer=='reference_cpp'
          else binding_frame(index,0,deleting=True))
    bpp_fpm.send(data); bpp.app_db.wait_for_deleted_entry(BINDING,bpp.po)
    bpp_fpm.send(data); assert not bpp.app_db.get_entry(BINDING,bpp.po)


@pytest.mark.parametrize('invalid',['zero_id','zero_index','negative_index','unknown_index','short','extra'])
def test_binding_rejects_invalid_input_without_unknown_key(bpp,bpp_fpm,invalid):
    index=bpp_fpm.ifindex()
    bpp_fpm.send(binding_frame(index,bpp.group_id))
    expected={'nexthop_group':str(bpp.group_id)}; exact(bpp,BINDING,bpp.po,expected)
    payload=struct.pack('=iI',index,bpp.group_id+1)
    data={'zero_id':binding_frame(index,0),'zero_index':binding_frame(0,1),
          'negative_index':binding_frame(-1,1),'unknown_index':binding_frame(2147483647,1),
          'short':frame(147,payload[:-1]),'extra':frame(147,payload+b'\x00'*4)}[invalid]
    bpp_fpm.send(data)
    bpp_fpm.barrier()
    stable(lambda: assert_binding_unchanged(bpp,expected))
    # A later valid frame proves malformed payloads did not kill the daemon.
    bpp_fpm.send(binding_frame(index,bpp.group_id+1))
    exact(bpp,BINDING,bpp.po,{'nexthop_group':str(bpp.group_id+1)})


def assert_binding_unchanged(bpp,expected):
    assert bpp.app_db.get_entry(BINDING,bpp.po)==expected
    assert 'unknown' not in bpp.app_db.get_keys(BINDING)


def test_binding_delete_after_interface_removal_and_stale_index_after_recreation(bpp,bpp_fpm):
    name='BppFpm999'
    run(bpp.dvs,['ip','link','add',name,'type','dummy'])
    try:
        old=int(run(bpp.dvs,['cat','/sys/class/net/'+name+'/ifindex']))
        bpp_fpm.send(binding_frame(old,bpp.group_id))
        exact(bpp,BINDING,name,{'nexthop_group':str(bpp.group_id)})
        run(bpp.dvs,['ip','link','del',name])
        bpp_fpm.send(binding_frame(old,0,deleting=True))
        bpp.app_db.wait_for_deleted_entry(BINDING,name)
        run(bpp.dvs,['ip','link','add',name,'type','dummy'])
        new=int(run(bpp.dvs,['cat','/sys/class/net/'+name+'/ifindex'])); assert new!=old
        bpp_fpm.send(binding_frame(new,bpp.group_id+1))
        exact(bpp,BINDING,name,{'nexthop_group':str(bpp.group_id+1)})
        bpp_fpm.send(binding_frame(old,0,deleting=True))
        stable(lambda: assert_dummy_binding(bpp,name,bpp.group_id+1))
    finally:
        bpp.delete_app(BINDING,name)
        bpp.dvs.runcmd(['ip','link','del',name])


def assert_dummy_binding(bpp,name,identifier):
    assert bpp.app_db.get_entry(BINDING,name)=={'nexthop_group':str(identifier)}


@pytest.mark.parametrize('ip',['10.10.10.2','2001:db8::2'])
@pytest.mark.parametrize('kind',[0,1])
@pytest.mark.parametrize('producer',['crafted','reference_cpp'])
def test_member_ipv4_ipv6_and_type_roundtrip_exact_rows(bpp,bpp_fpm,ip,kind,producer):
    identifier=0x4fffffff
    data=(bpp_fpm.encode('member',identifier,'add',kind,ip,bpp.mac) if producer=='reference_cpp'
          else member_frame(identifier,ip,kind=kind))
    bpp_fpm.send(data)
    if kind == 0:
        bpp_fpm.barrier()
        assert not bpp.app_db.get_entry(OBJECTS,str(identifier))
    else:
        exact(bpp,OBJECTS,str(identifier),member_row(ip))
        assert 'vni' not in bpp.app_db.get_entry(OBJECTS,str(identifier))
        assert bpp.app_db.get_entry(FPM_IDS,str(identifier))=={'NULL':'NULL'}
    bpp_fpm.send(member_frame(identifier,ip,kind=1))
    exact(bpp,OBJECTS,str(identifier),member_row(ip))
    # L2 over FPM must not replace an accepted routed Member.
    bpp_fpm.send(member_frame(identifier,ip,kind=0))
    bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,identifier,member_row(ip)))

    data=(bpp_fpm.encode('member',identifier,'del',kind,ip,bpp.mac) if producer=='reference_cpp'
          else member_frame(identifier,ip,kind=kind,deleting=True))
    bpp_fpm.send(data); bpp.app_db.wait_for_deleted_entry(OBJECTS,str(identifier))
    bpp_fpm.send(data)


@pytest.mark.parametrize('invalid',['short','extra','zero_id','type','family','zero_mac','multicast_mac'])
def test_member_invalid_payload_preserves_accepted_row(bpp,bpp_fpm,invalid):
    identifier=bpp.member_id
    bpp_fpm.send(member_frame(identifier,bpp.ip)); expected=member_row(bpp.ip)
    exact(bpp,OBJECTS,str(identifier),expected)
    payload=member_frame(identifier,'10.10.10.3')[20:]
    if invalid=='short': payload=payload[:-1]
    if invalid=='extra': payload+=b'\x00'*4
    if invalid=='zero_id': payload=b'\x00'*4+payload[4:]
    if invalid=='type': payload=payload[:4]+b'\x02'+payload[5:]
    if invalid=='family': payload=payload[:5]+b'\xff'+payload[6:]
    if invalid=='zero_mac': payload=payload[:-6]+b'\x00'*6
    if invalid=='multicast_mac': payload=payload[:-6]+bytes.fromhex('010000000002')
    bpp_fpm.send(frame(149,payload))
    bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,identifier,expected))
    assert not bpp.app_db.get_entry(OBJECTS,'0')
    bpp_fpm.send(member_frame(identifier,'10.10.10.3'))
    exact(bpp,OBJECTS,str(identifier),member_row('10.10.10.3'))


def assert_object_unchanged(bpp,identifier,expected):
    assert bpp.app_db.get_entry(OBJECTS,str(identifier))==expected


@pytest.mark.parametrize('producer',['crafted','reference_cpp'])
def test_group_unsigned_sort_empty_replacement_and_delete_roundtrip(bpp,bpp_fpm,producer):
    members=[0x40000001,0x40000007,0x4fffffff]
    data=(bpp_fpm.encode('group',bpp.group_id,'add',*reversed(members)) if producer=='reference_cpp'
          else group_frame(bpp.group_id,members))
    bpp_fpm.send(data)
    exact(bpp,OBJECTS,str(bpp.group_id),{'nexthop_group':','.join(map(str,members))})
    bpp_fpm.send(group_frame(bpp.group_id,[]))
    exact(bpp,OBJECTS,str(bpp.group_id),{'nexthop_group':''})
    data=(bpp_fpm.encode('group',bpp.group_id,'del') if producer=='reference_cpp'
          else group_frame(bpp.group_id,[],deleting=True))
    bpp_fpm.send(data); bpp.app_db.wait_for_deleted_entry(OBJECTS,str(bpp.group_id))
    bpp_fpm.send(data)


@pytest.mark.parametrize('invalid',['truncated_header','truncated_members','extra','over_count','zero_id','zero_member','duplicate','unsorted'])
def test_group_invalid_count_length_or_ids_preserve_row(bpp,bpp_fpm,invalid):
    bpp_fpm.send(group_frame(bpp.group_id,[bpp.member_id]))
    expected={'nexthop_group':str(bpp.member_id)}
    exact(bpp,OBJECTS,str(bpp.group_id),expected)
    data={
        'truncated_header':frame(151,struct.pack('=I',bpp.group_id)),
        'truncated_members':group_frame(bpp.group_id,[0x40000001],count=2),
        'extra':group_frame(bpp.group_id,[0x40000001],count=0),
        'over_count':group_frame(bpp.group_id,[],count=4097),
        'zero_id':group_frame(0,[0x40000001]),'zero_member':group_frame(bpp.group_id,[0]),
        'duplicate':group_frame(bpp.group_id,[0x40000001,0x40000001]),'unsorted':group_frame(bpp.group_id,[0x4fffffff,0x40000001]),
    }[invalid]
    bpp_fpm.send(data); bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,bpp.group_id,expected))
    assert not bpp.app_db.get_entry(OBJECTS,'0')
    bpp_fpm.send(group_frame(bpp.group_id,[0x40000001,0x4fffffff]))
    exact(bpp,OBJECTS,str(bpp.group_id),dict(expected,nexthop_group='1073741825,1342177279'))


@pytest.mark.parametrize('row_kind',['member','group'])
def test_foreign_producer_collision_add_delete_and_reconnect_preserve_row(bpp,bpp_fpm,row_kind):
    fields={'remote_vtep':'10.10.10.99'} if row_kind=='member' else {'nexthop_group':''}
    bpp.app(OBJECTS,str(bpp.member_id),fields)
    exact(bpp,OBJECTS,str(bpp.member_id),fields)
    bpp_fpm.send(member_frame(bpp.member_id,bpp.ip))
    bpp_fpm.send(member_frame(bpp.member_id,bpp.ip,deleting=True))
    bpp_fpm.send(group_frame(bpp.member_id,[1]))
    bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,bpp.member_id,fields))
    bpp_fpm.disconnect()
    replacement=FpmSession(bpp)
    try: stable(lambda: assert_object_unchanged(bpp,bpp.member_id,fields))
    finally: replacement.close()


def replay(bpp,session):
    # Binding and Group before Member exercise real transport dependency parking.
    session.send(session.encode('binding',session.ifindex(),'add',bpp.group_id))
    session.send(session.encode('group',bpp.group_id,'add',bpp.member_id))
    session.send(session.encode('member',bpp.member_id,'add',1,bpp.ip,bpp.mac))


@pytest.mark.parametrize('failure',['disconnect','crash'])
@pytest.mark.parametrize('admin',['AUTO','PRIMARY','PROTECTION'])
@pytest.mark.parametrize('revertive',[False,True])
def test_fpm_session_flush_keeps_config_and_rebuilds_only_from_replay(bpp,bpp_fpm,failure,admin,revertive):
    bpp.policy(admin,revertive,wtr=2000); replay(bpp,bpp_fpm)
    bpp.path('PROTECTION' if admin=='PROTECTION' else 'PRIMARY')
    saved=bpp.config.get_entry(CONFIG,bpp.po)
    old_group=bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]
    old_peers={key for key in bpp.asic.get_keys(NH) if bpp.asic.get_entry(NH,key).get('SAI_NEXT_HOP_ATTR_IP')==bpp.ip}
    old_tunnels={key for key in bpp.asic.get_keys(TUNNEL) if bpp.asic.get_entry(TUNNEL,key).get('SAI_TUNNEL_ATTR_ENCAP_DST_IP')==bpp.ip}
    assert old_peers and old_tunnels
    if failure=='crash':
        # Exercise startup cleanup from persisted accepted IDs, not a destructor.
        restart_fpmsyncd_after_sigkill(bpp.dvs)
        # SIGKILL bypasses destructors. Start a replacement via Supervisor; its
        # persisted rows must be flushed by the first pre-accept reset.
        bpp_fpm.disconnect()
    else: bpp_fpm.disconnect()
    bpp.app_db.wait_for_deleted_entry(BINDING,bpp.po)
    for identifier in (bpp.group_id,bpp.member_id):
        bpp.app_db.wait_for_deleted_entry(OBJECTS,str(identifier))
    bpp.asic.wait_for_deleted_entry(NHG,old_group)
    for oid in old_peers: bpp.asic.wait_for_deleted_entry(NH,oid)
    for oid in old_tunnels: bpp.asic.wait_for_deleted_entry(TUNNEL,oid)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state=None)
    assert bpp.config.get_entry(CONFIG,bpp.po)==saved
    assert admin in bpp.show()
    bpp.link(False)
    replacement=FpmSession(bpp)
    try:
        replay(bpp,replacement)
        expected='PRIMARY' if admin=='PRIMARY' else 'PROTECTION'
        bpp.path(expected); assert bpp.latest_log()['wtr_state']=='IDLE'
        assert bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR] in bpp.asic.get_keys(NHG)
        bpp.link(True)
        if admin=='AUTO' and revertive:
            bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')
        else: stable(lambda: assert_fpm_path(bpp,expected),duration=3)
    finally: replacement.close()


def test_same_session_wrong_object_kind_cannot_overwrite_or_delete(bpp,bpp_fpm):
    bpp_fpm.send(member_frame(bpp.member_id,bpp.ip))
    expected=member_row(bpp.ip); exact(bpp,OBJECTS,str(bpp.member_id),expected)
    bpp_fpm.send(group_frame(bpp.member_id,[1]))
    bpp_fpm.send(group_frame(bpp.member_id,[],deleting=True))
    bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,bpp.member_id,expected))


def test_cli_through_fpm_and_swss_show_reports_confirmed_readback(bpp,bpp_fpm):
    for field,value in [('protection_mode','software'),('admin_mode','auto'),
                        ('revertive','false'),('wait_to_restore_time_ms','5000')]:
        run(bpp.dvs,['config','portchannel999','protection',field,value])
    bpp.wait(admin_mode='AUTO')
    bpp.config.wait_for_field_match(CONFIG,bpp.po,{'revertive':'false','wait_to_restore_time_ms':'5000'})
    replay(bpp,bpp_fpm); bpp.path('PRIMARY')
    assert 'PRIMARY' in bpp.show() and 'STEADY' in bpp.show()
    bpp.link(False); bpp.path('PROTECTION')
    from dvslib.dvs_bridge_port_protection import parse_show
    values=parse_show(bpp.show(), bpp.po)
    assert values['ADMIN_MODE']=='AUTO'
    assert values['STATE'].strip()=='PROTECTION'
    assert 'PROGRAMMED_SAI_ADMIN_MODE' not in values
    for field in ['T_RECONCILE_ARMED_AT','LAST_ARBITRATION_SOURCE','LAST_CACHE_UPDATE']: assert field in values


def test_largest_transport_sized_group_is_decoded_without_count_wrap(bpp,bpp_fpm):
    # FPM(4) + aligned netlink/Group header(36) + 28 bytes per Member.
    members=[0x40000001+i for i in range(583)]
    data=group_frame(bpp.group_id,members)
    assert len(data)==16364 and len(data)<=16384
    bpp_fpm.send(data)
    exact(bpp,OBJECTS,str(bpp.group_id),{'nexthop_group':','.join(map(str,members))})


@pytest.mark.parametrize('object_kind',['member','group'])
def test_malformed_delete_cannot_withdraw_an_accepted_object(bpp,bpp_fpm,object_kind):
    if object_kind=='member':
        identifier=bpp.member_id; data=member_frame(identifier,bpp.ip)
        expected=member_row(bpp.ip); malformed=frame(150,data[20:-1])
    else:
        identifier=bpp.group_id; data=group_frame(identifier,[0x40000001])
        expected={'nexthop_group':'1073741825'}
        malformed=group_frame(identifier,[],deleting=True,count=1)
    bpp_fpm.send(data); exact(bpp,OBJECTS,str(identifier),expected)
    bpp_fpm.send(malformed); bpp_fpm.barrier()
    stable(lambda: assert_object_unchanged(bpp,identifier,expected))


@pytest.mark.parametrize('failure',['disconnect','crash'])
@pytest.mark.parametrize('ids',[(12001,12002,12003),(0xfffffffe,0xffffffff,0x80000001)])
def test_real_kernel_fdbsyncd_members_groups_and_asic_objects_survive_fpm_reset(bpp,bpp_fpm,failure,ids):
    # Actual kernel RTM_NEWNEXTHOP notifications -> actual fdbsyncd. Legacy
    # rows have ONLY remote_vtep or nexthop_group, not synthetic type/owner tags.
    first,second,group=ids; ips=['10.10.10.22','10.10.10.23']
    created=[]; legacy={}
    try:
        for identifier,ip in zip([first,second],ips):
            bpp.app('VXLAN_REMOTE_VNI_TABLE',bpp.vlan+':'+ip,{'vni':'99999'})
            poll(lambda: True if any(bpp.asic.get_entry(TUNNEL,k).get('SAI_TUNNEL_ATTR_ENCAP_DST_IP')==ip
                                    for k in bpp.asic.get_keys(TUNNEL)) else None)
            run(bpp.dvs,['ip','nexthop','add','id',str(identifier),'via',ip,'fdb'])
            created.append(identifier)
            legacy[str(identifier)]={'remote_vtep':ip}
            exact(bpp,OBJECTS,str(identifier),legacy[str(identifier)])
        run(bpp.dvs,['ip','nexthop','add','id',str(group),'group',str(first)+'/'+str(second),'fdb'])
        created.append(group); legacy[str(group)]={'nexthop_group':str(first)+','+str(second)}
        exact(bpp,OBJECTS,str(group),legacy[str(group)])
        member_table='ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER'
        def hardware():
            hops={oid:bpp.asic.get_entry(NH,oid) for oid in bpp.asic.get_keys(NH)
                  if bpp.asic.get_entry(NH,oid).get('SAI_NEXT_HOP_ATTR_IP') in ips}
            members={oid:bpp.asic.get_entry(member_table,oid) for oid in bpp.asic.get_keys(member_table)
                     if bpp.asic.get_entry(member_table,oid).get('SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID') in hops}
            groups={attrs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID'] for attrs in members.values()}
            if len(hops)!=2 or len(members)!=2 or len(groups)!=1: return None
            tunnel_ids={attrs['SAI_NEXT_HOP_ATTR_TUNNEL_ID'] for attrs in hops.values()}
            return {NH:hops,member_table:members,
                    NHG:{oid:bpp.asic.get_entry(NHG,oid) for oid in groups},
                    TUNNEL:{oid:bpp.asic.get_entry(TUNNEL,oid) for oid in tunnel_ids}}
        snapshot=poll(hardware)
        bpp.policy(); replay(bpp,bpp_fpm); bpp.path('PRIMARY')
        # Both FPM object kinds and operations must reject EVERY legacy key.
        for identifier in ids:
            bpp_fpm.send(member_frame(identifier,bpp.ip))
            bpp_fpm.send(member_frame(identifier,bpp.ip,deleting=True))
            bpp_fpm.send(group_frame(identifier,[bpp.member_id]))
            bpp_fpm.send(group_frame(identifier,[],deleting=True))
        bpp_fpm.barrier()
        if failure=='crash': restart_fpmsyncd_after_sigkill(bpp.dvs)
        bpp_fpm.disconnect()
        bpp.app_db.wait_for_deleted_entry(BINDING,bpp.po)
        for identifier in [bpp.group_id,bpp.member_id]:
            bpp.app_db.wait_for_deleted_entry(OBJECTS,str(identifier))
            bpp.app_db.wait_for_deleted_entry(FPM_IDS,str(identifier))
        bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state=None)
        def unchanged():
            for key,row in legacy.items(): assert bpp.app_db.get_entry(OBJECTS,key)==row
            for table,rows in snapshot.items():
                for oid,attrs in rows.items(): assert bpp.asic.get_entry(table,oid)==attrs
        stable(unchanged,duration=2)
        assert bpp.config.get_entry(CONFIG,bpp.po)['admin_mode']=='AUTO'
        # fdbsyncd's own cache must still drive legitimate group maintenance.
        run(bpp.dvs,['ip','nexthop','del','id',str(first)]); created.remove(first)
        bpp.app_db.wait_for_deleted_entry(OBJECTS,str(first))
        exact(bpp,OBJECTS,str(group),{'nexthop_group':str(second)})
    finally:
        for identifier in reversed(created):
            run(bpp.dvs,['ip','nexthop','del','id',str(identifier)])
            bpp.app_db.wait_for_deleted_entry(OBJECTS,str(identifier))
        for ip in ips: bpp.delete_app('VXLAN_REMOTE_VNI_TABLE',bpp.vlan+':'+ip)


@pytest.mark.parametrize('missing_members',[False,True])
def test_hld_empty_or_orphan_group_flushes_after_fpm_crash_without_row_markers(bpp,bpp_fpm,missing_members):
    bpp.policy(); before=set(bpp.asic.get_keys(NHG))
    members=[bpp.member_id] if missing_members else []
    bpp_fpm.send(group_frame(bpp.group_id,members))
    exact(bpp,OBJECTS,str(bpp.group_id),{'nexthop_group':','.join(map(str,members))})
    exact(bpp,FPM_IDS,str(bpp.group_id),{'NULL':'NULL'})
    assert not bpp.app_db.get_entry(OBJECTS,str(bpp.member_id))
    restart_fpmsyncd_after_sigkill(bpp.dvs); bpp_fpm.disconnect()
    bpp.app_db.wait_for_deleted_entry(OBJECTS,str(bpp.group_id))
    bpp.app_db.wait_for_deleted_entry(FPM_IDS,str(bpp.group_id))
    poll(lambda: True if set(bpp.asic.get_keys(NHG))==before else None)
    assert bpp.config.get_entry(CONFIG,bpp.po)['admin_mode']=='AUTO'


def test_replay_done_is_informational_and_keeps_confirmed_path(bpp,bpp_fpm):
    bpp.policy(); replay(bpp,bpp_fpm); bpp.path('PRIMARY')
    before=bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]
    bpp_fpm.send(frame(153,b'')); bpp_fpm.send(frame(153,b''))
    bpp_fpm.barrier(); assert_fpm_path(bpp,'PRIMARY')
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]==before
    bpp_fpm.send(frame(153,b'\x00'*4)); bpp_fpm.barrier()
    assert_fpm_path(bpp,'PRIMARY')


def test_group_with_invalid_last_member_publishes_no_partial_snapshot(bpp,bpp_fpm):
    first,second=0x40002001,0x40002002
    valid=group_frame(bpp.group_id,[first,second])
    # Last Member RMAC is invalid; the earlier Member must not be published.
    payload=valid[20:20+18+2*28]
    bpp_fpm.send(frame(151,payload[:-6]+b'\x00'*6)); bpp_fpm.barrier()
    for identifier in (first,second,bpp.group_id):
        assert not bpp.app_db.get_entry(OBJECTS,str(identifier))
    bpp_fpm.send(valid)
    exact(bpp,OBJECTS,str(bpp.group_id),{'nexthop_group':str(first)+','+str(second)})
