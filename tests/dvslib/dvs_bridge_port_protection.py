"""Protection DVS fixtures using the existing full DVS framework.

Runtime tests inject aggregate LAG_TABLE events, as test_portchannel.py does.
They test real daemon/DB/SAI integration, not physical LACP/min-links convergence.
FPM tests retain a real TCP session to fpmsyncd. Faults use a DVS-only interposer;
there are no new production hooks and no direct writes to STATE_DB/ASIC_DB.
"""
import base64
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import time
import uuid

import pytest
from swsscommon import swsscommon

CONFIG = 'BRIDGE_PORT_PROTECTION'
STATE = 'BRIDGE_PORT_PROTECTION_STATE'
BINDING = 'EVPN_ES_BACKUP_NHG_TABLE'
OBJECTS = 'L2_NEXTHOP_GROUP_TABLE'
FPM_IDS = '_FPM_PROTECTION_OBJECT_IDS'
BP = 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT'
NHG = 'ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP'
NHGM = 'ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER'
NH = 'ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP'
TUNNEL = 'ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL'
GROUP_ATTR = 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID'
MODE_ATTR = 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE'
ADMIN_ATTR = 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE'


def run(dvs, command):
    rc, output = dvs.runcmd(command)
    assert rc == 0, (command, output)
    return output


def parse_show(output, interface):
    # Real CLI invocations can include dependency warnings before the table.
    # Match its actual header and require a complete, unique interface row.
    lines = [line.split() for line in output.splitlines() if line.split()]
    headers = [line for line in lines if line[0] == 'INTERFACE' and 'ADMIN_MODE' in line]
    rows = [line for line in lines if line[0] == interface]
    assert len(headers) == len(rows) == 1, output
    assert len(headers[0]) == len(rows[0]), output
    return dict(zip(headers[0], rows[0]))


def atomic_file(dvs, path, data):
    script = ('import base64,pathlib,sys; p=pathlib.Path(sys.argv[1]); '
              't=p.with_suffix(".tmp"); t.write_bytes(base64.b64decode(sys.argv[2])); t.replace(p)')
    run(dvs, ['python3', '-c', script, path, base64.b64encode(data.encode()).decode()])


def poll(function, timeout=20):
    # Docker RPCs can be slow under emulation. Bound elapsed time rather than
    # multiplying a nominal iteration count by the cost of every RPC.
    deadline = time.monotonic()+timeout
    while True:
        value = function()
        if value is not None:
            return value
        remaining = deadline-time.monotonic()
        assert remaining > 0, f'Operation timed out after {timeout} seconds'
        time.sleep(min(0.02, remaining))


def restart_fpmsyncd_after_sigkill(dvs):
    old_pid = int(run(dvs,['supervisorctl','pid','fpmsyncd']).strip())
    assert old_pid > 0
    run(dvs,['supervisorctl','signal','KILL','fpmsyncd'])

    def restarted():
        _, status = dvs.runcmd(['supervisorctl','status','fpmsyncd'])
        # The standard DVS intentionally disables autorestart. Start the
        # replacement explicitly after observing the killed process exit.
        if any(state in status.split() for state in ['EXITED','STOPPED','FATAL']):
            run(dvs,['supervisorctl','start','fpmsyncd'])
        elif 'RUNNING' not in status:
            return None
        pid = int(run(dvs,['supervisorctl','pid','fpmsyncd']).strip())
        return pid if pid > 0 and pid != old_pid else None

    return poll(restarted)


def stable(check, duration=1):
    # Negative assertions require a sustained window, not a single early read.
    end = time.monotonic()+duration
    while time.monotonic() < end:
        check()
        time.sleep(0.02)


def frame(kind, payload, nl_flags=0x401):
    data = struct.pack('=IHHII', 16+len(payload), kind, nl_flags, 0, 0)+payload
    data += b'\x00' * (-len(data) % 4)
    return struct.pack('!BBH', 1, 1, len(data)+4)+data


def binding_frame(index, group, deleting=False):
    return frame(148 if deleting else 147, struct.pack('=iI', index, group))


def member_frame(identifier, ip, mac='02:00:00:00:00:02', kind=1, deleting=False):
    family = 10 if ':' in ip else 2
    import socket
    address = socket.inet_pton(family, ip).ljust(16,b'\x00')
    return frame(150 if deleting else 149, struct.pack('=IBB',identifier,kind,family)+address+bytes.fromhex(mac.replace(':','')))


def group_frame(identifier, members, deleting=False, count=None):
    count = len(members) if count is None else count
    return frame(152 if deleting else 151, struct.pack('=II',identifier,count)+b''.join(struct.pack('=I',m) for m in members))


class InstrumentedDVS:
    """Install the opt-in test interposer only in this module's DVS container."""
    def __init__(self, dvs):
        self.dvs = dvs
        directory = os.environ.get('BPP_DVS_ARTIFACT_DIR')
        assert directory, 'Build tests/dvs_test_tools/build_artifacts.py and set BPP_DVS_ARTIFACT_DIR before DVS execution'
        self.artifacts = Path(directory)
        for name in ('manifest.json','libbpp_dvs_fault.so','fpm_encoder'):
            assert (self.artifacts/name).is_file(), str(self.artifacts/name)
        self.manifest = json.loads((self.artifacts/'manifest.json').read_text())
        self.constants = self.manifest['constants']
        self.directory = '/tmp/bpp-dvs-'+uuid.uuid4().hex
        run(dvs, ['mkdir','-p',self.directory])
        dvs.copy_file(self.directory,str(self.artifacts/'libbpp_dvs_fault.so'))
        client = Path(__file__).resolve().parents[1]/'dvs_test_tools/fpm_client.py'
        dvs.copy_file(self.directory,str(client))
        self.control = self.directory+'/fault'
        self.trace_path = self.directory+'/trace.jsonl'
        atomic_file(dvs,self.control,'target=0\n')
        script = '''import configparser,glob,json,shutil,sys
for path in glob.glob('/etc/supervisor/conf.d/*.conf'):
 p=configparser.ConfigParser(interpolation=None); p.read(path)
 if p.has_section('program:orchagent'):
  backup=path+'.bpp_backup'; shutil.copy2(path,backup)
  command=p.get('program:orchagent','command',raw=True)
  prefix='/usr/bin/env LD_PRELOAD='+sys.argv[1]+' BPP_DVS_SAI_FAULT_FILE='+sys.argv[2]+' BPP_DVS_SAI_TRACE_FILE='+sys.argv[3]+' '
  p.set('program:orchagent','command',prefix+command)
  with open(path,'w') as out: p.write(out)
  print(json.dumps([path,backup])); break
else: raise RuntimeError('No orchagent supervisor program found')
'''
        self.supervisor = json.loads(run(dvs,['python3','-c',script,self.directory+'/libbpp_dvs_fault.so',self.control,self.trace_path]))
        # Empty test configuration; use the standard cold DVS restart, not warm
        # adoption or a hand-started second orchagent.
        dvs.restart()
        assert 'RUNNING' in run(dvs,['supervisorctl','status','orchagent'])
        # Rosetta can expose the translator as /proc/<pid>/exe, defeating
        # pidof's executable-name lookup. Supervisor owns this exact process.
        pid = int(run(dvs,['supervisorctl','pid','orchagent']).strip())
        assert pid > 0, 'Supervisor has no running orchagent PID'
        maps = run(dvs,['cat',f'/proc/{pid}/maps'])
        assert 'libbpp_dvs_fault.so' in maps, 'Fault/trace artifact not loaded into actual orchagent'

    def close(self):
        atomic_file(self.dvs,self.control,'target=0\n')
        path,backup = self.supervisor
        run(self.dvs,['cp',backup,path]); run(self.dvs,['rm','-f',backup])
        self.dvs.restart()
        run(self.dvs,['rm','-rf',self.directory])


class ProtectionDVS:
    po = 'PortChannel999'
    vlan = 'Vlan999'
    source_name = 'BppVtep'
    nvo = 'BppNvo'
    group_id = 0x80001001
    member_id = 0x40001001
    ip = '10.10.10.2'
    mac = '02:00:00:00:00:02'
    def __init__(self, instrumented, real_lag=False):
        self.instrumented = instrumented
        self.dvs = instrumented.dvs
        self.config = self.dvs.get_config_db()
        self.app_db = self.dvs.get_app_db()
        self.asic = self.dvs.get_asic_db()
        self.state_db = self.dvs.get_state_db()
        self.app_rows = set()
        self.log_after = 0
        self.source_created = False
        self.map_created = False
        self.interface_ips = set()
        assert not self.config.get_entry('PORTCHANNEL',self.po)
        assert not self.config.get_entry(CONFIG,self.po)
        assert not self.config.get_entry('VXLAN_EVPN_NVO',self.nvo)
        run(self.dvs,['config','portchannel','add',self.po]+(['--fallback','true'] if real_lag else []))
        self.config.wait_for_entry('PORTCHANNEL',self.po)
        run(self.dvs,['config','vlan','add','999'])
        self.bridge(True)
        if not real_lag:
            self.link(True)
        self.source()

    def app(self, table, key, fields):
        self.mark_logs()
        producer = swsscommon.ProducerStateTable(self.app_db.db_connection,table)
        producer.set(key,swsscommon.FieldValuePairs(list(fields.items())))
        self.app_rows.add((table,key))

    def record_fpm_id(self, key):
        # Simulate fpmsyncd's private accepted-key bookkeeping before SET.
        swsscommon.Table(self.app_db.db_connection,FPM_IDS).set(key,swsscommon.FieldValuePairs([('NULL','NULL')]))

    def delete_app(self, table, key):
        self.mark_logs()
        producer = swsscommon.ProducerStateTable(self.app_db.db_connection,table)
        producer._del(key)
        self.app_rows.discard((table,key))
        if table == OBJECTS:
            self.app_db.wait_for_deleted_entry(OBJECTS,key)
            swsscommon.Table(self.app_db.db_connection,FPM_IDS)._del(key)

    def source(self, ip='10.10.10.1'):
        self.config.create_entry('VXLAN_TUNNEL',self.source_name,{'src_ip':ip})
        self.config.create_entry('VXLAN_TUNNEL_MAP',self.source_name+'|bpp_map',{'vni':'99999','vlan':self.vlan})
        self.config.create_entry('VXLAN_EVPN_NVO',self.nvo,{'source_vtep':self.source_name})
        self.source_created = self.map_created = True
        # IMET input is dropped if it beats NVO setup. Wait for the real
        # consumer and source tunnel before submitting dependent test input.
        self.app_db.wait_for_field_match('VXLAN_EVPN_NVO_TABLE',self.nvo,{'source_vtep':self.source_name})
        poll(lambda: True if any(self.asic.get_entry(TUNNEL,key).get('SAI_TUNNEL_ATTR_ENCAP_SRC_IP')==ip
                                for key in self.asic.get_keys(TUNNEL)) else None)

    def bridge(self, present):
        if present:
            run(self.dvs,['config','vlan','member','add','999',self.po])
            # An existing bridge can be pending retirement. Its mere presence
            # does not acknowledge the new VLAN-member SET.
            poll(lambda: self.bridge_oid() if self.bridge_vlan_members(self.bridge_oid()) else None)
        else:
            old = self.bridge_oid()
            run(self.dvs,['config','vlan','member','del','999',self.po])
            poll(lambda: True if not self.bridge_vlan_members(old) else None)
            return old

    def bridge_vlan_members(self, bridge):
        table='ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER'
        return [key for key in self.asic.get_keys(table)
                if self.asic.get_entry(table,key).get('SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID')==bridge]

    def interface_ip(self, prefix):
        run(self.dvs,['config','interface','ip','add',self.vlan,prefix])
        self.interface_ips.add(prefix)

    def bridge_oid(self):
        counters = self.dvs.get_counters_db().get_entry('COUNTERS_LAG_NAME_MAP','')
        lag = counters.get(self.po)
        if not lag: return None
        matches = [oid for oid in self.asic.get_keys(BP)
                   if self.asic.get_entry(BP,oid).get('SAI_BRIDGE_PORT_ATTR_PORT_ID') == lag]
        assert len(matches) <= 1, matches
        return matches[0] if matches else None

    def link(self, up):
        self.app('LAG_TABLE',self.po,{'admin_status':'up','mtu':'9100','oper_status':'up' if up else 'down'})
        self.app_db.wait_for_field_match('LAG_TABLE',self.po,{'oper_status':'up' if up else 'down'})

    def policy(self, admin='AUTO', revertive=False, wtr=0):
        self.mark_logs()
        self.config.create_entry(CONFIG,self.po,{'protection_mode':'SOFTWARE','admin_mode':admin,
            'revertive':str(revertive).lower(),'wait_to_restore_time_ms':str(wtr)})
        self.state_db.wait_for_field_match(STATE,self.po,{'admin_mode':admin})
        self.config.wait_for_field_match(CONFIG,self.po,{'revertive':str(revertive).lower(),'wait_to_restore_time_ms':str(wtr)})

    def member(self, identifier=None, ip=None, mac=None, kind='l3'):
        identifier = identifier or self.member_id
        self.record_fpm_id(str(identifier))
        self.app(OBJECTS,str(identifier),{'type':kind,'remote_vtep':ip or self.ip,'router_mac':self.mac if mac is None else mac})

    def group(self, members=None, identifier=None):
        identifier = identifier or self.group_id
        members = [self.member_id] if members is None else members
        self.record_fpm_id(str(identifier))
        self.app(OBJECTS,str(identifier),{'nexthop_group':','.join(map(str,members))})

    def bind(self, group=None):
        self.app(BINDING,self.po,{'nexthop_group':str(group or self.group_id)})

    def state(self):
        return self.state_db.get_entry(STATE,self.po)

    def events(self):
        # Observe actual daemon logs, never synthesize STATE_DB diagnostics.
        output = run(self.dvs,['cat','/var/log/syslog'])
        rows=[]
        for line in output.splitlines():
            if 'bpProtOrch' in line and 'ifname='+self.po in line and 'reconciliation_state=' in line and 'wtr_state=' in line:
                rows.append(dict(re.findall(r'(\w+)=([^\s]*)',line)))
        return rows

    def terminal_errors(self):
        output = run(self.dvs,['cat','/var/log/syslog'])
        return [dict(re.findall(r'(\w+)=([^\s]*)',line)) for line in output.splitlines()
                if 'bpProtOrch' in line and 'ifname='+self.po in line and 'event=sai_terminal_failure' in line]

    def mark_logs(self):
        self.log_after=len(self.events())

    def latest_log(self):
        return poll(lambda: self.events()[-1] if self.events() else None)

    def wait_log(self, **fields):
        aliases={'last_event':'event','desired_sai_admin_mode':'desired_admin',
                 'programmed_sai_admin_mode':'programmed_admin','pending_dependencies':'dependencies'}
        def found():
            rows=self.events()[self.log_after:]
            for row in reversed(rows):
                if all(row.get(aliases.get(key,key))==value for key,value in fields.items()): return row
            return None
        return poll(found)

    def wait(self, **fields):
        def matched():
            actual=self.state()
            return actual if actual and all((key not in actual if value is None else actual.get(key)==value)
                                           for key,value in fields.items()) else None
        return poll(matched)

    def wait_runtime(self, **fields):
        diagnostics={'last_event','desired_sai_admin_mode','programmed_sai_admin_mode',
                     'pending_dependencies','wtr_state','wtr_deadline_monotonic_ms',
                     'retry_attempts','retry_at_monotonic_ms','retry_backoff_capped'}
        logged={key:value for key,value in fields.items() if key in diagnostics}
        public={key:value for key,value in fields.items() if key not in diagnostics}
        result=self.wait(**public)
        if logged: self.wait_log(**logged)
        return result

    def start(self, admin='AUTO', revertive=False, wtr=0):
        self.policy(admin,revertive,wtr); self.member(); self.group(); self.bind()
        self.path('PROTECTION' if admin == 'PROTECTION' else 'PRIMARY')

    def path(self, expected):
        result = self.wait(protection_state=expected,reconciliation_state='STEADY')
        oid = self.bridge_oid(); assert oid
        self.asic.wait_for_field_match(BP,oid,{MODE_ATTR:'SAI_BRIDGE_PORT_PROTECTION_MODE_SOFTWARE',
            ADMIN_ATTR:'SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_'+expected})
        assert self.asic.get_entry(BP,oid).get(GROUP_ATTR,'oid:0x0') != 'oid:0x0'
        self.fault()  # Bind trace capture to this current incarnation.
        return result

    def fault(self, fail='', state=None, status='failure', oid=None, group_oid=None, mismatch='', member_oid=None, tunnel_oid=None):
        current = oid or self.bridge_oid()
        target = int(current.split(':',1)[1],16) if current else 0
        text = f'target={hex(target)}\nfail={fail}\nstatus={status}\nmismatch={mismatch}\n'
        if member_oid: text += 'member='+member_oid.split(':',1)[1]+'\n'
        if tunnel_oid: text += 'tunnel='+tunnel_oid.split(':',1)[1]+'\n'
        if current:
            group = group_oid or self.asic.get_entry(BP,current).get(GROUP_ATTR,'oid:0x0')
            text += f'group={group.split(":",1)[1]}\n'
        if state is not None: text += f'state={self.instrumented.constants["state_"+state.lower()]}\n'
        atomic_file(self.dvs,self.instrumented.control,text)

    def trace(self):
        rc, output = self.dvs.runcmd(['cat',self.instrumented.trace_path])
        if rc: return []
        return [json.loads(line) for line in output.splitlines() if line.strip()]

    def admin_writes(self, since=0):
        attr = self.instrumented.constants['attr_admin']
        return [x for x in self.trace()[since:] if x['operation']=='set' and x['attr']==attr]

    def show(self):
        return run(self.dvs,['show','bridge-port-protection',self.po,'--verbose'])

    def close(self):
        self.fault()
        self.config.delete_entry(CONFIG,self.po)
        self.state_db.wait_for_deleted_entry(STATE,self.po)
        # Association must clear before table/resource teardown.
        self.delete_app(BINDING,self.po)
        for table,key in list(self.app_rows):
            if table == OBJECTS and 'nexthop_group' in self.app_db.get_entry(table,key):
                self.delete_app(table,key)
        for table,key in list(self.app_rows):
            if table == OBJECTS: self.delete_app(table,key)
        # Also remove FPM rows authored through the socket, if still present.
        for identifier in (self.group_id,self.group_id+1,self.member_id,self.member_id+1):
            self.delete_app(OBJECTS,str(identifier))
        for table,key in list(self.app_rows):
            if table != 'LAG_TABLE': self.delete_app(table,key)
        for prefix in self.interface_ips:
            self.config.delete_entry('VLAN_INTERFACE',self.vlan+'|'+prefix)
        if self.interface_ips:
            self.config.delete_entry('VLAN_INTERFACE',self.vlan)
        if self.source_created:
            self.config.delete_entry('VXLAN_EVPN_NVO',self.nvo)
            self.config.delete_entry('VXLAN_TUNNEL_MAP',self.source_name+'|bpp_map')
            self.config.delete_entry('VXLAN_TUNNEL',self.source_name)
        self.config.delete_entry('VLAN_MEMBER',self.vlan+'|'+self.po)
        poll(lambda: True if not self.bridge_oid() else None)
        self.delete_app('LAG_TABLE',self.po)
        run(self.dvs,['config','portchannel','del',self.po])
        # Match DVSVlan.remove_vlan: the host CLI's final-VLAN cleanup invokes
        # Docker to manage ndppd, which is outside this single-container DVS.
        self.config.delete_entry('VLAN',self.vlan)
        self.app_db.wait_for_deleted_entry('VLAN_TABLE',self.vlan)


class FpmSession:
    def __init__(self, bpp):
        self.bpp = bpp; self.dvs = bpp.dvs
        self.directory = bpp.instrumented.directory+'/fpm-'+uuid.uuid4().hex
        self.sequence = 0; self.closed = False
        _, status = self.dvs.runcmd(['supervisorctl','status','zebra'])
        if 'RUNNING' in status: run(self.dvs,['supervisorctl','stop','zebra'])
        run(self.dvs,['mkdir','-p',self.directory])
        script = bpp.instrumented.directory+'/fpm_client.py'
        run(self.dvs,['sh','-c',f'python3 {script} {self.directory} >{self.directory}/client.log 2>&1 &'])
        poll(lambda: self.ack().get('connected') or None)

    def ack(self):
        rc, output = self.dvs.runcmd(['cat',self.directory+'/ack.json'])
        return json.loads(output) if rc==0 else {}

    def send(self, data):
        self.sequence += 1
        atomic_file(self.dvs,self.directory+'/command.json',json.dumps({'sequence':self.sequence,'frame':base64.b64encode(data).decode()}))
        result = poll(lambda: value if (value:=self.ack()).get('sequence')==self.sequence else None)
        assert result.get('sent'), result

    def encode(self, *arguments):
        return subprocess.check_output([str(self.bpp.instrumented.artifacts/'fpm_encoder'),*map(str,arguments)])

    def barrier(self):
        # TCP ordering plus a distinct Group SET/DEL acknowledgement proves
        # fpmsyncd consumed preceding malformed/no-op frames before asserting.
        marker=0x800020ff
        self.send(group_frame(marker,[]))
        self.bpp.app_db.wait_for_field_match(OBJECTS,str(marker),{'nexthop_group':''})
        self.send(group_frame(marker,[],deleting=True))
        self.bpp.app_db.wait_for_deleted_entry(OBJECTS,str(marker))

    def ifindex(self):
        return int(run(self.dvs,['cat','/sys/class/net/'+self.bpp.po+'/ifindex']).strip())

    def disconnect(self):
        if self.closed: return
        self.sequence += 1
        atomic_file(self.dvs,self.directory+'/command.json',json.dumps({'sequence':self.sequence,'close':True}))
        poll(lambda: self.ack().get('closed') or None)
        self.closed = True

    def close(self):
        self.disconnect()
        _, status = self.dvs.runcmd(['supervisorctl','status','zebra'])
        if 'RUNNING' not in status: run(self.dvs,['supervisorctl','start','zebra'])


@pytest.fixture(scope='module')
def bpp_instrumented(dvs):
    instance = InstrumentedDVS(dvs)
    try: yield instance
    finally: instance.close()


@pytest.fixture
def bpp(bpp_instrumented, request):
    try:
        instance = ProtectionDVS(bpp_instrumented)
    except Exception:
        bpp_instrumented.dvs.restart()
        raise
    trace_start = len(instance.trace())
    try: yield instance
    finally:
        directory = os.environ.get('BPP_DVS_DIAGNOSTICS_DIR')
        if directory:
            # Capture before teardown clears the fault and confirmed state.
            path = Path(directory)/hashlib.sha256(request.node.nodeid.encode()).hexdigest()[:16]
            path.mkdir(parents=True,exist_ok=True)
            try:
                snapshot = dict(nodeid=request.node.nodeid,state=instance.state(),
                    trace=instance.trace()[trace_start:],events=instance.events()[-50:],
                    fault=run(instance.dvs,['cat',instance.instrumented.control]))
            except Exception as error:
                snapshot = dict(nodeid=request.node.nodeid,diagnostic_error=str(error))
            (path/'observations.json').write_text(json.dumps(snapshot,indent=2)+'\n')
        try:
            instance.close()
        except Exception:
            # Report teardown failure, but reset this owned testbed before the
            # next case so leaked state cannot turn one failure into a cascade.
            instance.dvs.restart()
            raise


@pytest.fixture
def bpp_fpm(bpp):
    session = FpmSession(bpp)
    try: yield session
    finally: session.close()
