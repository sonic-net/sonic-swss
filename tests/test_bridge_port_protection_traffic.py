"""Real packet learning and protection lifecycle checks.

Packets traverse the DVS veth/TAP/kernel path. These cases do not assert backup
VXLAN/ECMP delivery: the default libVS backend does not implement that pipeline.
"""
import json
import os
import re
import subprocess
from pathlib import Path
import pytest
from dvslib.dvs_bridge_port_protection import (
    bpp_instrumented, ProtectionDVS, FpmSession, CONFIG, STATE, BP,
    GROUP_ATTR, BINDING, OBJECTS, NHG, NHGM, TUNNEL,
    run, poll, stable,
)

FDB = 'ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'


class TrafficPortChannel(ProtectionDVS):
    mac_address = '02:99:00:00:00:02'
    host_ip = '192.0.2.2'

    def __init__(self, instrumented):
        super().__init__(instrumented, real_lag=True)
        self.server = self.dvs.servers[0]
        self.packet_results = []
        self.saved_link = json.loads(self.server.runcmd_output('ip -j link show dev eth0'))[0]
        assert not any(a['family']=='inet' for a in json.loads(self.server.runcmd_output('ip -j addr show dev eth0'))[0]['addr_info']), 'Traffic fixture requires an unused server'
        # The single-member LACP fallback exercises the real Linux team and
        # teamsyncd status path, without synthetic LAG_TABLE oper-status writes.
        run(self.dvs,['config','portchannel','member','add',self.po,'Ethernet0'])
        run(self.dvs,['config','interface','startup','Ethernet0'])
        self.config.update_entry('VLAN',self.vlan,{'admin_status':'up'})
        self.interface_ip('192.0.2.1/24')
        # Keep NUD aging independent of the protection lifecycle under test,
        # following the existing warm-reboot DVS reachable-time setup. Learn
        # and confirm through packets; never insert a static neighbor row.
        run(self.dvs,['sysctl','-w','net.ipv4.neigh.'+self.vlan+'.base_reachable_time_ms=300000'])
        self.host('ip link set dev eth0 down')
        self.host('ip link set dev eth0 address '+self.mac_address)
        self.host('ip link add link eth0 name eth0.999 type vlan id 999')
        self.host('ip addr add 192.0.2.2/24 dev eth0.999')
        self.set_link(True)
        self.host('ip link set dev eth0.999 up')

    def host(self, command):
        assert self.server.runcmd(command)==0, command

    def set_link(self, up):
        self.mark_logs()
        self.host('ip link set dev eth0 '+('up' if up else 'down'))
        run(self.dvs,['config','interface','startup' if up else 'shutdown','Ethernet0'])
        self.app_db.wait_for_field_match('LAG_TABLE',self.po,{'oper_status':'up' if up else 'down'})
        assert run(self.dvs,['cat','/sys/class/net/'+self.po+'/operstate']).strip()==('up' if up else 'down')

    def ping(self):
        self.probe(self.server,'192.0.2.1')

    def probe(self,server,destination):
        command = 'ping -n -c 3 -W 2 -I eth0.999 '+destination
        try:
            output = server.runcmd_output(command)
        except subprocess.CalledProcessError as error:
            output = error.output.decode(errors='replace') if isinstance(error.output,bytes) else str(error.output)
            self.packet_results.append({'destination':destination,'namespace':server.nsname,
                                        'command':command,'output':output,'returncode':error.returncode})
            raise AssertionError(output) from error
        self.packet_results.append({'destination':destination,'namespace':server.nsname,
                                    'command':command,'output':output})
        assert re.search(r'3 packets transmitted, 3 received, 0% packet loss',output),output

    def learned(self):
        def find():
            vlans = {key for key in self.asic.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_VLAN')
                     if self.asic.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_VLAN',key).get('SAI_VLAN_ATTR_VLAN_ID')=='999'}
            assert len(vlans)==1,vlans
            rows = {key:self.asic.get_entry(FDB,key) for key in self.asic.get_keys(FDB)
                    if json.loads(key).get('mac','').lower()==self.mac_address
                    and json.loads(key).get('bvid') in vlans}
            if len(rows)!=1:
                return None
            key,attrs = next(iter(rows.items()))
            assert attrs['SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID']==self.bridge_oid(), rows
            assert attrs['SAI_FDB_ENTRY_ATTR_TYPE']=='SAI_FDB_ENTRY_TYPE_DYNAMIC', rows
            return key,attrs
        return poll(find)

    def preserved(self, entry):
        key,attrs = entry
        assert self.asic.get_entry(FDB,key)==attrs

    def learn(self):
        self.ping()
        command = ['ping','-n','-c','3','-W','2','-I',self.vlan,self.host_ip]
        output = run(self.dvs,command)
        self.packet_results.append({'destination':self.host_ip,'namespace':'switch',
                                    'command':command,'output':output})
        assert re.search(r'3 packets transmitted, 3 received, 0% packet loss',output),output
        poll(lambda: True if 'REACHABLE' in self.kernel_neighbor().get('state',[]) else None)
        entry = self.learned()
        table = 'ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY'
        def find_neighbor():
            rows = {key:self.asic.get_entry(table,key) for key in self.asic.get_keys(table)
                    if json.loads(key).get('ip')==self.host_ip}
            if len(rows)==1:
                key,attrs = next(iter(rows.items()))
                assert attrs['SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS'].lower()==self.mac_address
                return key,attrs
            return None
        self.neighbor = poll(find_neighbor)
        self.initial_neighbor = self.kernel_neighbor()
        return entry

    def kernel_neighbor(self):
        rows = json.loads(run(self.dvs,['ip','-j','neigh','show','to',self.host_ip,'dev',self.vlan]))
        return rows[0] if len(rows)==1 else {}

    def primary_objects_preserved(self, entry):
        self.preserved(entry)
        key,attrs = self.neighbor
        actual = self.asic.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY',key)
        assert actual==attrs,{'expected':attrs,'actual':actual,'kernel':self.kernel_neighbor(),
                             'app':self.app_db.get_entry('NEIGH_TABLE',self.vlan+':'+self.host_ip)}

    def no_flush(self, since):
        assert not [event for event in self.trace()[since:]
                    if event['operation']=='fdb_flush' and event['status']==0]

    def group_oid(self):
        return self.asic.get_entry(BP,self.bridge_oid())[GROUP_ATTR]

    def restore_server(self):
        self.host('ip link set dev eth0 down')
        if self.server.runcmd('ip link show dev eth0.999')==0:
            self.host('ip link del dev eth0.999')
        self.host('ip link set dev eth0 address '+self.saved_link['address'])
        self.host('ip link set dev eth0 '+('up' if 'UP' in self.saved_link['flags'] else 'down'))

    def close_traffic(self):
        self.restore_server()
        run(self.dvs,['config','portchannel','member','del',self.po,'Ethernet0'])
        self.config.wait_for_deleted_entry('PORTCHANNEL_MEMBER',self.po+'|Ethernet0')
        poll(lambda: True if not self.app_db.get_keys('LAG_MEMBER_TABLE') else None)
        self.close()


@pytest.fixture
def traffic(bpp_instrumented,request):
    instance = TrafficPortChannel.__new__(TrafficPortChannel)
    try:
        instance.__init__(bpp_instrumented)
    except Exception:
        try:
            if hasattr(instance,'saved_link'):
                instance.restore_server()
        finally:
            bpp_instrumented.dvs.restart()
        raise
    try:
        yield instance
    finally:
        directory = os.environ.get('BPP_DVS_DIAGNOSTICS_DIR')
        if directory:
            path = Path(directory)/'traffic'
            path.mkdir(parents=True,exist_ok=True)
            try:
                snapshot = {'nodeid':request.node.nodeid,'state':instance.state(),
                            'events':instance.events()[-50:],'trace':instance.trace(),
                            'team':instance.dvs.runcmd(['teamdctl',instance.po,'state','dump']),
                            'links':instance.dvs.runcmd(['ip','-j','link']),
                            'addresses':instance.dvs.runcmd(['ip','-j','addr']),
                            'vlans':instance.dvs.runcmd(['bridge','vlan','show']),
                            'server':instance.server.runcmd_output('ip -j addr'),
                            'packets':instance.packet_results,
                            'initial_neighbor':getattr(instance,'initial_neighbor',{}),
                            'kernel_neighbor':instance.kernel_neighbor(),
                            'fdb':{key:instance.asic.get_entry(FDB,key) for key in instance.asic.get_keys(FDB)}}
            except Exception as error:
                snapshot = {'nodeid':request.node.nodeid,'diagnostic_error':str(error)}
            (path/(request.node.name+'.json')).write_text(json.dumps(snapshot,indent=2)+'\n')
        try:
            instance.close_traffic()
        except Exception:
            bpp_instrumented.dvs.restart()
            raise


def test_primary_packets_learn_on_real_portchannel(traffic):
    traffic.start()
    traffic.learn()
    assert traffic.state()['reconciliation_state']=='STEADY'


def test_cli_policy_updates_keep_primary_packets_and_learned_entries(traffic):
    entry = traffic.learn()
    for field,value in [('protection_mode','software'),('admin_mode','primary'),
                        ('revertive','true'),('wait_to_restore_time_ms','3000')]:
        run(traffic.dvs,['config','portchannel999','protection',field,value])
    traffic.member()
    traffic.group()
    traffic.bind()
    traffic.path('PRIMARY')
    assert 'PRIMARY' in traffic.show() and 'STEADY' in traffic.show()
    before = traffic.config.get_entry(CONFIG,traffic.po)
    rc,_ = traffic.dvs.runcmd(['config','portchannel999','protection','wait_to_restore_time_ms','-1'])
    assert rc!=0
    assert traffic.config.get_entry(CONFIG,traffic.po)==before
    traffic.primary_objects_preserved(entry)
    traffic.ping()


def test_bridged_packets_recover_after_revertive_failover(traffic):
    server = traffic.dvs.servers[1]
    saved = json.loads(server.runcmd_output('ip -j link show dev eth0'))[0]
    assert not any(a['family']=='inet' for a in json.loads(server.runcmd_output('ip -j addr show dev eth0'))[0]['addr_info'])
    def host(command):
        assert server.runcmd(command)==0,command
    try:
        run(traffic.dvs,['config','vlan','member','add','999','Ethernet4'])
        run(traffic.dvs,['config','interface','startup','Ethernet4'])
        host('ip link set dev eth0 down')
        host('ip link set dev eth0 address 02:99:00:00:00:03')
        host('ip link add link eth0 name eth0.999 type vlan id 999')
        host('ip addr add 192.0.2.3/24 dev eth0.999')
        host('ip link set dev eth0 up')
        host('ip link set dev eth0.999 up')
        traffic.start(revertive=True,wtr=3000)
        entry = traffic.learn()
        traffic.probe(traffic.server,'192.0.2.3')
        traffic.probe(server,'192.0.2.2')
        traffic.set_link(False)
        traffic.path('PROTECTION')
        traffic.primary_objects_preserved(entry)
        traffic.set_link(True)
        traffic.wait_log(wtr_state='WAIT_TO_RESTORE')
        traffic.path('PRIMARY')
        traffic.probe(traffic.server,'192.0.2.3')
        traffic.probe(server,'192.0.2.2')
        traffic.primary_objects_preserved(entry)
    finally:
        host('ip link set dev eth0 down')
        if server.runcmd('ip link show dev eth0.999')==0:
            host('ip link del dev eth0.999')
        host('ip link set dev eth0 address '+saved['address'])
        host('ip link set dev eth0 '+('up' if 'UP' in saved['flags'] else 'down'))
        run(traffic.dvs,['config','vlan','member','del','999','Ethernet4'])


@pytest.mark.parametrize('admin',['AUTO','PRIMARY','PROTECTION'])
@pytest.mark.parametrize('revertive',[False,True])
def test_real_link_down_retains_learned_entries_and_recovery(traffic,admin,revertive):
    entry = traffic.learn()
    traffic.start(admin,revertive,wtr=3000)
    since = len(traffic.trace())
    traffic.set_link(False)
    traffic.path('PRIMARY' if admin=='PRIMARY' else 'PROTECTION')
    stable(lambda: traffic.primary_objects_preserved(entry))
    traffic.set_link(True)
    expected = 'PRIMARY' if admin=='PRIMARY' or (admin=='AUTO' and revertive) else 'PROTECTION'
    if admin=='AUTO' and revertive:
        traffic.wait_log(wtr_state='WAIT_TO_RESTORE')
    traffic.path(expected)
    traffic.primary_objects_preserved(entry)
    traffic.no_flush(since)
    # Real packet delivery is asserted only after selecting PRIMARY. The
    # kernel TAP datapath cannot prove SAI backup forwarding or its isolation.
    traffic.policy('PRIMARY')
    traffic.path('PRIMARY')
    traffic.ping()


@pytest.mark.parametrize('intent',['policy','binding'])
def test_pending_intent_retains_traffic_learned_entries(traffic,intent):
    entry = traffic.learn()
    if intent=='policy':
        traffic.policy()
    else:
        traffic.bind()
    traffic.fault()
    since = len(traffic.trace())
    traffic.set_link(False)
    stable(lambda: traffic.primary_objects_preserved(entry))
    traffic.no_flush(since)
    traffic.set_link(True)
    traffic.start('PRIMARY')
    traffic.ping()
    traffic.primary_objects_preserved(entry)


def test_unprotected_real_link_down_flushes_learned_fdb(traffic):
    entry = traffic.learn()
    traffic.fault()
    since = len(traffic.trace())
    traffic.set_link(False)
    traffic.asic.wait_for_deleted_entry(FDB,entry[0])
    poll(lambda: True if any(event['operation']=='fdb_flush' and event['status']==0
                            for event in traffic.trace()[since:]) else None)
    traffic.set_link(True)
    traffic.learn()


def test_explicit_vlan_withdrawal_flushes_traffic_learned_fdb(traffic):
    traffic.start()
    entry = traffic.learn()
    traffic.bridge(False)
    traffic.asic.wait_for_deleted_entry(FDB,entry[0])


@pytest.mark.parametrize('config_first',[False,True])
def test_last_intent_withdrawal_flushes_learned_fdb_only_when_down(traffic,config_first):
    entry = traffic.learn()
    traffic.start()
    traffic.set_link(False)
    traffic.path('PROTECTION')
    since = len(traffic.trace())
    policy_del = lambda: traffic.config.delete_entry(CONFIG,traffic.po)
    binding_del = lambda: traffic.delete_app(BINDING,traffic.po)
    first,second = (policy_del,binding_del) if config_first else (binding_del,policy_del)
    first()
    stable(lambda: traffic.primary_objects_preserved(entry))
    traffic.no_flush(since)
    second()
    traffic.asic.wait_for_deleted_entry(FDB,entry[0])
    traffic.set_link(True)
    traffic.learn()


def test_real_flaps_restart_wtr_and_preserve_learned_entries(traffic):
    entry = traffic.learn()
    traffic.start(revertive=True,wtr=5000)
    since = len(traffic.trace())
    traffic.set_link(False)
    traffic.path('PROTECTION')
    traffic.set_link(True)
    first = int(traffic.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
    traffic.set_link(False)
    traffic.wait_log(wtr_state='IDLE')
    traffic.set_link(True)
    second = int(traffic.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
    assert second>first
    traffic.path('PRIMARY')
    traffic.primary_objects_preserved(entry)
    traffic.no_flush(since)
    traffic.ping()


@pytest.mark.parametrize('fault',['admin','get'])
def test_sai_retry_preserves_learned_entries_and_recovers_primary_packets(traffic,fault):
    entry = traffic.learn()
    traffic.start()
    since = len(traffic.trace())
    traffic.fault(fail=fault)
    traffic.set_link(False)
    traffic.wait(reconciliation_state='INCONSISTENT')
    stable(lambda: traffic.primary_objects_preserved(entry))
    traffic.no_flush(since)
    traffic.fault()
    traffic.path('PROTECTION')
    traffic.set_link(True)
    traffic.policy('PRIMARY')
    traffic.path('PRIMARY')
    traffic.ping()
    traffic.primary_objects_preserved(entry)


def test_terminal_attachment_failure_keeps_learned_entries(traffic):
    entry = traffic.learn()
    traffic.policy()
    traffic.bind()
    traffic.fault(fail='group',status='unsupported')
    traffic.member()
    traffic.group()
    traffic.wait_log(last_event='sai_unsupported')
    since = len(traffic.trace())
    traffic.set_link(False)
    stable(lambda: traffic.primary_objects_preserved(entry))
    traffic.no_flush(since)
    traffic.fault()
    traffic.set_link(True)
    traffic.policy('PRIMARY')
    traffic.path('PRIMARY')
    traffic.ping()


def test_member_churn_and_rmac_update_keep_primary_packet_learning(traffic):
    traffic.start()
    entry = traffic.learn()
    oid = traffic.group_oid()
    traffic.member(traffic.member_id+1,'10.10.10.3','02:00:00:00:00:03')
    traffic.group([traffic.member_id,traffic.member_id+1])
    def members():
        return [key for key in traffic.asic.get_keys(NHGM)
                if traffic.asic.get_entry(NHGM,key).get('SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID')==oid]
    poll(lambda: True if len(members())==2 else None)
    traffic.member(mac='02:00:00:00:00:22')
    poll(lambda: True if any(traffic.asic.get_entry(TUNNEL,key).get('SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC')=='02:00:00:00:00:22'
                            for key in traffic.asic.get_keys(TUNNEL)) else None)
    traffic.group([traffic.member_id+1])
    poll(lambda: True if len(members())==1 else None)
    traffic.delete_app(OBJECTS,str(traffic.member_id))
    assert traffic.group_oid()==oid
    traffic.primary_objects_preserved(entry)
    traffic.ping()


@pytest.mark.parametrize('down',[False,True])
def test_fpm_disconnect_and_ordered_replay_preserve_learned_entries(traffic,down):
    entry = traffic.learn()
    traffic.policy()
    session = FpmSession(traffic)
    try:
        def replay(peer):
            peer.send(peer.encode('member',traffic.member_id,'add',1,traffic.ip,traffic.mac))
            peer.send(peer.encode('group',traffic.group_id,'add',traffic.member_id))
            peer.send(peer.encode('binding',peer.ifindex(),'add',traffic.group_id))
        replay(session)
        traffic.path('PRIMARY')
        old = traffic.group_oid()
        old_peers = {key for key in traffic.asic.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP')
                     if traffic.asic.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP',key).get('SAI_NEXT_HOP_ATTR_IP')==traffic.ip}
        old_tunnels = {key for key in traffic.asic.get_keys(TUNNEL)
                       if traffic.asic.get_entry(TUNNEL,key).get('SAI_TUNNEL_ATTR_ENCAP_DST_IP')==traffic.ip}
        assert old_peers and old_tunnels
        if down:
            traffic.set_link(False)
            traffic.path('PROTECTION')
        since = len(traffic.trace())
        session.disconnect()
        traffic.app_db.wait_for_deleted_entry(BINDING,traffic.po)
        traffic.asic.wait_for_deleted_entry(NHG,old)
        for key in old_peers:
            traffic.asic.wait_for_deleted_entry('ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP',key)
        for key in old_tunnels:
            traffic.asic.wait_for_deleted_entry(TUNNEL,key)
        stable(lambda: traffic.primary_objects_preserved(entry))
        assert traffic.config.get_entry(CONFIG,traffic.po)
        assert 'AUTO' in traffic.show()
        # Accepted recovery caveat: wait for observed retirement before
        # replay; independent-session coalescing is not certified here.
        replacement = FpmSession(traffic)
        try:
            replay(replacement)
            if down:
                traffic.path('PROTECTION')
                traffic.set_link(True)
                traffic.policy('PRIMARY')
            traffic.path('PRIMARY')
            traffic.primary_objects_preserved(entry)
            traffic.no_flush(since)
            traffic.ping()
        finally:
            replacement.close()
    finally:
        session.close()
