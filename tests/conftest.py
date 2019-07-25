import os
import os.path
import re
import time
import json
import redis
import docker
import pytest
import commands
import tarfile
import StringIO
import subprocess
from datetime import datetime
from swsscommon import swsscommon

def ensure_system(cmd):
    rc = os.WEXITSTATUS(os.system(cmd))
    if rc:
        raise RuntimeError('Failed to run command: %s' % cmd)

def pytest_addoption(parser):
    parser.addoption("--dvsname", action="store", default=None,
                      help="dvs name")
    parser.addoption("--keeptb", action="store_true", default=False,
                      help="keep testbed after test")

class AsicDbValidator(object):
    def __init__(self, dvs):
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # get default dot1q vlan id
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

        keys = atbl.getKeys()
        assert len(keys) == 1
        self.default_vlan_id = keys[0]

        # build port oid to front port name mapping
        self.portoidmap = {}
        self.portnamemap = {}
        self.hostifoidmap = {}
        self.hostifnamemap = {}
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        keys = atbl.getKeys()

        assert len(keys) == 32
        for k in keys:
            (status, fvs) = atbl.get(k)

            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_HOSTIF_ATTR_OBJ_ID":
                    port_oid = fv[1]
                elif fv[0] == "SAI_HOSTIF_ATTR_NAME":
                    port_name = fv[1]

            self.portoidmap[port_oid] = port_name
            self.portnamemap[port_name] = port_oid
            self.hostifoidmap[k] = port_name
            self.hostifnamemap[port_name] = k

        # get default acl table and acl rules
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()

        assert len(keys) >= 1
        self.default_acl_tables = keys

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        assert len(keys) == 2
        self.default_acl_entries = keys

class ApplDbValidator(object):
    def __init__(self, dvs):
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.neighTbl = swsscommon.Table(appl_db, "NEIGH_TABLE")

    def __del__(self):
        # Make sure no neighbors on physical interfaces
        keys = self.neighTbl.getKeys()
        for key in keys:
            m = re.match("eth(\d+)", key)
            if not m:
                continue
            assert int(m.group(1)) > 0

class VirtualServer(object):
    def __init__(self, ctn_name, pid, i):
        self.nsname = "%s-srv%d" % (ctn_name, i)
        self.pifname = "eth%d" % (i + 1)
        self.cleanup = True

        # create netns
        if os.path.exists("/var/run/netns/%s" % self.nsname):
            self.cleanup = False
        else:
            ensure_system("ip netns add %s" % self.nsname)

            # create vpeer link
            ensure_system("ip link add %s type veth peer name %s" % (self.nsname[0:12], self.pifname))
            ensure_system("ip link set %s netns %s" % (self.nsname[0:12], self.nsname))
            ensure_system("ip link set %s netns %d" % (self.pifname, pid))

            # bring up link in the virtual server
            ensure_system("ip netns exec %s ip link set dev %s name eth0" % (self.nsname, self.nsname[0:12]))
            ensure_system("ip netns exec %s ip link set dev eth0 up" % (self.nsname))
            ensure_system("ip netns exec %s ethtool -K eth0 tx off" % (self.nsname))

            # bring up link in the virtual switch
            ensure_system("nsenter -t %d -n ip link set dev %s up" % (pid, self.pifname))

            # disable arp, so no neigh on physical interfaces
            ensure_system("nsenter -t %d -n ip link set arp off dev %s" % (pid, self.pifname))
            ensure_system("nsenter -t %d -n sysctl -w net.ipv6.conf.%s.disable_ipv6=1" % (pid, self.pifname))

    def destroy(self):
        if self.cleanup:
            pids = subprocess.check_output("ip netns pids %s" % (self.nsname), shell=True)
            if pids:
                for pid in pids.split('\n'):
                    if len(pid) > 0:
                        os.system("kill %s" % int(pid))
            ensure_system("ip netns delete %s" % self.nsname)

    def runcmd(self, cmd):
        try:
            out = subprocess.check_output("ip netns exec %s %s" % (self.nsname, cmd), stderr=subprocess.STDOUT, shell=True)
        except subprocess.CalledProcessError as e:
            print "------rc={} for cmd: {}------".format(e.returncode, e.cmd)
            print e.output.rstrip()
            print "------"
            return e.returncode
        return 0

    def runcmd_async(self, cmd):
        return subprocess.Popen("ip netns exec %s %s" % (self.nsname, cmd), shell=True)

    def runcmd_output(self, cmd):
        return subprocess.check_output("ip netns exec %s %s" % (self.nsname, cmd), shell=True)

class DockerVirtualSwitch(object):
    def __init__(self, name=None, keeptb=False, fakeplatform=None):
        self.basicd = ['redis-server',
                       'rsyslogd']
        self.swssd = ['orchagent',
                      'intfmgrd',
                      'neighsyncd',
                      'portsyncd',
                      'vlanmgrd',
                      'vrfmgrd',
                      'portmgrd']
        self.syncd = ['syncd']
        self.rtd   = ['fpmsyncd', 'zebra']
        self.teamd = ['teamsyncd', 'teammgrd']
        self.alld  = self.basicd + self.swssd + self.syncd + self.rtd + self.teamd
        self.client = docker.from_env()

        if subprocess.check_call(["/sbin/modprobe", "team"]) != 0:
            raise NameError("cannot install kernel team module")

        self.ctn = None
        if keeptb:
            self.cleanup = False
        else:
            self.cleanup = True
        if name != None:
            # get virtual switch container
            for ctn in self.client.containers.list():
                if ctn.name == name:
                    self.ctn = ctn
                    (status, output) = commands.getstatusoutput("docker inspect --format '{{.HostConfig.NetworkMode}}' %s" % name)
                    ctn_sw_id = output.split(':')[1]
                    self.cleanup = False
            if self.ctn == None:
                raise NameError("cannot find container %s" % name)

            # get base container
            for ctn in self.client.containers.list():
                if ctn.id == ctn_sw_id or ctn.name == ctn_sw_id:
                    ctn_sw_name = ctn.name

            (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % ctn_sw_name)
            self.ctn_sw_pid = int(output)

            # create virtual servers
            self.servers = []
            for i in range(32):
                server = VirtualServer(ctn_sw_name, self.ctn_sw_pid, i)
                self.servers.append(server)

            self.mount = "/var/run/redis-vs/{}".format(ctn_sw_name)

            self.net_cleanup()
            self.restart()
        else:
            self.ctn_sw = self.client.containers.run('debian:jessie', privileged=True, detach=True,
                    command="bash", stdin_open=True)
            (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % self.ctn_sw.name)
            self.ctn_sw_pid = int(output)

            # create virtual server
            self.servers = []
            for i in range(32):
                server = VirtualServer(self.ctn_sw.name, self.ctn_sw_pid, i)
                self.servers.append(server)

            # mount redis to base to unique directory
            self.mount = "/var/run/redis-vs/{}".format(self.ctn_sw.name)
            os.system("mkdir -p {}".format(self.mount))

            self.environment = ["fake_platform={}".format(fakeplatform)] if fakeplatform else []

            # create virtual switch container
            self.ctn = self.client.containers.run('docker-sonic-vs', privileged=True, detach=True,
                    environment=self.environment,
                    network_mode="container:%s" % self.ctn_sw.name,
                    volumes={ self.mount: { 'bind': '/var/run/redis', 'mode': 'rw' } })

        self.appldb = None
        self.redis_sock = self.mount + '/' + "redis.sock"
        try:
            # temp fix: remove them once they are moved to vs start.sh
            self.ctn.exec_run("sysctl -w net.ipv6.conf.default.disable_ipv6=0")
            for i in range(0, 128, 4):
                self.ctn.exec_run("sysctl -w net.ipv6.conf.eth%d.disable_ipv6=1" % (i + 1))
            self.check_ready()
            self.init_asicdb_validator()
            self.appldb = ApplDbValidator(self)
        except:
            self.destroy()
            raise

    def destroy(self):
        if self.appldb:
            del self.appldb
        if self.cleanup:
            self.ctn.remove(force=True)
            self.ctn_sw.remove(force=True)
            os.system("rm -rf {}".format(self.mount))
            for s in self.servers:
                s.destroy()

    def check_ready(self, timeout=30):
        '''check if all processes in the dvs is ready'''

        re_space = re.compile('\s+')
        process_status = {}
        ready = False
        started = 0
        while True:
            # get process status
            res = self.ctn.exec_run("supervisorctl status")
            try:
                out = res.output
            except AttributeError:
                out = res
            for l in out.split('\n'):
                fds = re_space.split(l)
                if len(fds) < 2:
                    continue
                process_status[fds[0]] = fds[1]

            # check if all processes are running
            ready = True
            for pname in self.alld:
                try:
                    if process_status[pname] != "RUNNING":
                        ready = False
                except KeyError:
                    ready = False

            # check if start.sh exited
            if process_status["start.sh"] != "EXITED":
                ready = False

            if ready == True:
                break

            started += 1
            if started > timeout:
                raise ValueError(out)

            time.sleep(1)

    def net_cleanup(self):
        """clean up network, remove extra links"""

        re_space = re.compile('\s+')

        res = self.ctn.exec_run("ip link show")
        try:
            out = res.output
        except AttributeError:
            out = res
        for l in out.split('\n'):
            m = re.compile('^\d+').match(l)
            if not m:
                continue
            fds = re_space.split(l)
            if len(fds) > 1:
                pname = fds[1].rstrip(":")
                m = re.compile("(eth|lo|Bridge|Ethernet)").match(pname)
                if not m:
                    self.ctn.exec_run("ip link del {}".format(pname))
                    print "remove extra link {}".format(pname)
        return

    def restart(self):
        self.ctn.restart()

    # start processes in SWSS
    def start_swss(self):
        cmd = ""
        for pname in self.swssd:
            cmd += "supervisorctl start {}; ".format(pname)
        self.runcmd(['sh', '-c', cmd])

    # stop processes in SWSS
    def stop_swss(self):
        cmd = ""
        for pname in self.swssd:
            cmd += "supervisorctl stop {}; ".format(pname)
        self.runcmd(['sh', '-c', cmd])

    def start_zebra(dvs):
        dvs.runcmd(['sh', '-c', 'supervisorctl start zebra'])

        # Let's give zebra a chance to connect to FPM.
        time.sleep(5)

    def stop_zebra(dvs):
        dvs.runcmd(['sh', '-c', 'pkill -9 zebra'])
        time.sleep(1)

    def start_fpmsyncd(dvs):
        dvs.runcmd(['sh', '-c', 'supervisorctl start fpmsyncd'])

        # Let's give fpmsyncd a chance to connect to Zebra.
        time.sleep(5)

    def stop_fpmsyncd(dvs):
        dvs.runcmd(['sh', '-c', 'pkill -x fpmsyncd'])
        time.sleep(1)

    def init_asicdb_validator(self):
        self.asicdb = AsicDbValidator(self)

    def runcmd(self, cmd):
        res = self.ctn.exec_run(cmd)
        try:
            exitcode = res.exit_code
            out = res.output
        except AttributeError:
            exitcode = 0
            out = res
        if exitcode != 0:
            print "-----rc={} for cmd {}-----".format(exitcode, cmd)
            print out.rstrip()
            print "-----"

        return (exitcode, out)

    def copy_file(self, path, filename):
        tarstr = StringIO.StringIO()
        tar = tarfile.open(fileobj=tarstr, mode="w")
        tar.add(filename, os.path.basename(filename))
        tar.close()
        self.ctn.exec_run("mkdir -p %s" % path)
        self.ctn.put_archive(path, tarstr.getvalue())
        tarstr.close()

    def get_logs(self, modname=None):
        stream, stat = self.ctn.get_archive("/var/log/")
        if modname == None:
            log_dir = "log"
        else:
            log_dir = "log/{}".format(modname)
        os.system("rm -rf {}".format(log_dir))
        os.system("mkdir -p {}".format(log_dir))
        p = subprocess.Popen(["tar", "--no-same-owner", "-C", "./{}".format(log_dir), "-x"], stdin=subprocess.PIPE)
        for x in stream:
            p.stdin.write(x)
        p.stdin.close()
        p.wait()
        if p.returncode:
            raise RuntimeError("Failed to unpack the archive.")
        os.system("chmod a+r -R log")

    def add_log_marker(self, file=None):
        marker = "=== start marker {} ===".format(datetime.now().isoformat())

        if file:
            self.runcmd(['sh', '-c', "echo \"{}\" >> {}".format(marker, file)])
        else:
            self.ctn.exec_run("logger {}".format(marker))

        return marker

    def SubscribeAppDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.APPL_DB)
        pubsub = r.pubsub()
        pubsub.psubscribe("__keyspace@0__:%s*" % objpfx)
        return pubsub

    def SubscribeAsicDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB)
        pubsub = r.pubsub()
        pubsub.psubscribe("__keyspace@1__:ASIC_STATE:%s*" % objpfx)
        return pubsub

    def CountSubscribedObjects(self, pubsub, ignore=None, timeout=10):
        nadd = 0
        ndel = 0
        idle = 0
        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                print message
                if ignore:
                    fds = message['channel'].split(':')
                    if fds[2] in ignore:
                        continue
                if message['data'] == 'hset':
                    nadd += 1
                elif message['data'] == 'del':
                    ndel += 1
                idle = 0
            else:
                time.sleep(1)
                idle += 1

        return (nadd, ndel)

    def GetSubscribedAppDbObjects(self, pubsub, ignore=None, timeout=10):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.APPL_DB)

        addobjs = []
        delobjs = []
        idle = 0
        prev_key = None

        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                print message
                key = message['channel'].split(':', 1)[1]
                # In producer/consumer_state_table scenarios, every entry will
                # show up twice for every push/pop operation, so skip the second
                # one to avoid double counting.
                if key != None and key == prev_key:
                    continue
                # Skip instructions with meaningless keys. To be extended in the
                # future to other undesired keys.
                if key == "ROUTE_TABLE_KEY_SET" or key == "ROUTE_TABLE_DEL_SET":
                    continue
                if ignore:
                    fds = message['channel'].split(':')
                    if fds[2] in ignore:
                        continue

                if message['data'] == 'hset':
                    (_, k) = key.split(':', 1)
                    value=r.hgetall(key)
                    addobjs.append({'key':json.dumps(k), 'vals':json.dumps(value)})
                    prev_key = key
                elif message['data'] == 'del':
                    (_, k) = key.split(':', 1)
                    delobjs.append({'key':json.dumps(k)})
                idle = 0
            else:
                time.sleep(1)
                idle += 1

        return (addobjs, delobjs)


    def GetSubscribedAsicDbObjects(self, pubsub, ignore=None, timeout=10):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB)

        addobjs = []
        delobjs = []
        idle = 0

        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                print message
                key = message['channel'].split(':', 1)[1]
                if ignore:
                    fds = message['channel'].split(':')
                    if fds[2] in ignore:
                        continue
                if message['data'] == 'hset':
                    value=r.hgetall(key)
                    (_, t, k) = key.split(':', 2)
                    addobjs.append({'type':t, 'key':k, 'vals':value})
                elif message['data'] == 'del':
                    (_, t, k) = key.split(':', 2)
                    delobjs.append({'key':k})
                idle = 0
            else:
                time.sleep(1)
                idle += 1

        return (addobjs, delobjs)

    def SubscribeDbObjects(self, dbobjs):
        # assuming all the db object pairs are in the same db instance
        r = redis.Redis(unix_socket_path=self.redis_sock)
        pubsub = r.pubsub()
        substr = ""
        for db, obj in dbobjs:
            pubsub.psubscribe("__keyspace@{}__:{}".format(db, obj))
        return pubsub

    def GetSubscribedMessages(self, pubsub, timeout=10):
        messages = []
        delobjs = []
        idle = 0
        prev_key = None

        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                messages.append(message)
                idle = 0
            else:
                time.sleep(1)
                idle += 1
        return (messages)

    def get_map_iface_bridge_port_id(self, asic_db):
        port_id_2_iface = self.asicdb.portoidmap
        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        iface_2_bridge_port_id = {}
        for key in tbl.getKeys():
            status, data = tbl.get(key)
            assert status
            values = dict(data)
            iface_id = values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]
            iface_name = port_id_2_iface[iface_id]
            iface_2_bridge_port_id[iface_name] = key

        return iface_2_bridge_port_id

    def is_table_entry_exists(self, db, table, keyregex, attributes):
        tbl = swsscommon.Table(db, table)
        keys = tbl.getKeys()

        extra_info = []
        for key in keys:
            if re.match(keyregex, key) is None:
                continue

            status, fvs = tbl.get(key)
            assert status, "Error reading from table %s" % table

            d_attributes = dict(attributes)
            for k, v in fvs:
                if k in d_attributes and d_attributes[k] == v:
                    del d_attributes[k]

            if len(d_attributes) != 0:
                extra_info.append("Desired attributes %s was not found for key %s" % (str(d_attributes), key))
            else:
                return True, extra_info
        else:
            if not extra_info:
                extra_info.append("Desired key regex %s was not found" % str(keyregex))
            return False, extra_info

    def all_table_entry_has(self, db, table, keyregex, attributes):
        tbl = swsscommon.Table(db, table)
        keys = tbl.getKeys()
        extra_info = []

        if len(keys) == 0:
            extra_info.append("keyregex %s not found" % keyregex)
            return False, extra_info

        for key in keys:
            if re.match(keyregex, key) is None:
                continue

            status, fvs = tbl.get(key)
            assert status, "Error reading from table %s" % table

            d_attributes = dict(attributes)
            for k, v in fvs:
                if k in d_attributes and d_attributes[k] == v:
                    del d_attributes[k]

            if len(d_attributes) != 0:
                extra_info.append("Desired attributes %s were not found for key %s" % (str(d_attributes), key))
                return False, extra_info

        return True, extra_info

    def all_table_entry_has_no(self, db, table, keyregex, attributes_list):
        tbl = swsscommon.Table(db, table)
        keys = tbl.getKeys()
        extra_info = []

        if len(keys) == 0:
            extra_info.append("keyregex %s not found" % keyregex)
            return False, extra_info

        for key in keys:
            if re.match(keyregex, key) is None:
                continue

            status, fvs = tbl.get(key)
            assert status, "Error reading from table %s" % table

            for k, v in fvs:
                if k in attributes_list:
                    extra_info.append("Unexpected attribute %s was found for key %s" % (k, key))
                    return False, extra_info

        return True, extra_info

    def is_fdb_entry_exists(self, db, table, key_values, attributes):
        tbl =  swsscommon.Table(db, table)
        keys = tbl.getKeys()

        exists = False
        extra_info = []
        key_found = False
        for key in keys:
            try:
                d_key = json.loads(key)
            except ValueError:
                d_key = json.loads('{' + key + '}')

            for k, v in key_values:
                if k not in d_key or v != d_key[k]:
                    continue

            key_found = True

            status, fvs = tbl.get(key)
            assert status, "Error reading from table %s" % table

            d_attributes = dict(attributes)
            for k, v in fvs:
                if k in d_attributes and d_attributes[k] == v:
                    del d_attributes[k]

            if len(d_attributes) != 0:
                exists = False
                extra_info.append("Desired attributes %s was not found for key %s" % (str(d_attributes), key))
            else:
                exists = True
                break

        if not key_found:
            exists = False
            extra_info.append("Desired key with parameters %s was not found" % str(key_values))

        return exists, extra_info

    def create_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set("Vlan" + vlan, fvs)
        time.sleep(1)

    def remove_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    def create_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)

    def create_vlan_member_tagged(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "tagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)

    def remove_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    def set_interface_status(self, interface, admin_status):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", admin_status)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface, fvs)
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip);
        time.sleep(1)

    def set_mtu(self, interface, mtu):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("mtu", mtu)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4")])
        tbl.set(interface + ":" + ip, fvs)
        time.sleep(1)

    def setup_db(self):
        self.pdb = swsscommon.DBConnector(0, self.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, self.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, self.redis_sock, 0)

    def getCrmCounterValue(self, key, counter):
        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, self.redis_sock, 0)
        crm_stats_table = swsscommon.Table(counters_db, 'CRM')

        for k in crm_stats_table.get(key)[1]:
            if k[0] == counter:
                return int(k[1])

    def setReadOnlyAttr(self, obj, attr, val):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, self.redis_sock, 0)
        tbl = swsscommon.Table(db, "ASIC_STATE:{0}".format(obj))
        keys = tbl.getKeys()

        assert len(keys) == 1

        swVid = keys[0]
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB)
        swRid = r.hget("VIDTORID", swVid)

        assert swRid is not None

        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "true", fvp)
        fvp = swsscommon.FieldValuePairs([(attr, val)])
        key = "SAI_OBJECT_TYPE_SWITCH:" + swRid

        ntf.send("set_ro", key, fvp)

@pytest.yield_fixture(scope="module")
def dvs(request):
    name = request.config.getoption("--dvsname")
    keeptb = request.config.getoption("--keeptb")
    fakeplatform = getattr(request.module, "DVS_FAKE_PLATFORM", None)
    dvs = DockerVirtualSwitch(name, keeptb, fakeplatform)
    yield dvs
    if name == None:
        dvs.get_logs(request.module.__name__)
    else:
        dvs.get_logs()
    dvs.destroy()

@pytest.yield_fixture
def testlog(request, dvs):
    dvs.runcmd("logger === start test %s ===" % request.node.name)
    yield testlog
    dvs.runcmd("logger === finish test %s ===" % request.node.name)
