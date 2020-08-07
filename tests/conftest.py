import os
import os.path
import re
import time
import json
import redis
import docker
import pytest
import tarfile
import io
import subprocess
import sys
if sys.version_info < (3, 0):
    import commands

from datetime import datetime
from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig
from dvslib.dvs_acl import DVSAcl
from dvslib import dvs_vlan
from dvslib import dvs_lag
from dvslib import dvs_mirror
from dvslib import dvs_policer

# FIXME: For the sake of stabilizing the PR pipeline we currently assume there are 32 front-panel
# ports in the system (much like the rest of the test suite). This should be adjusted to accomodate
# a dynamic number of ports. GitHub Issue: Azure/sonic-swss#1384.
NUM_PORTS = 32


def ensure_system(cmd):
    if sys.version_info < (3, 0):
        (rc, output) = commands.getstatusoutput(cmd)
    else:
        (rc, output) = subprocess.getstatusoutput(cmd)
    if rc:
        raise RuntimeError('Failed to run command: %s. rc=%d. output: %s' % (cmd, rc, output))


def pytest_addoption(parser):
    parser.addoption("--dvsname", action="store", default=None,
                      help="dvs name")
    parser.addoption("--keeptb", action="store_true", default=False,
                      help="keep testbed after test")
    parser.addoption("--imgname", action="store", default="docker-sonic-vs",
                      help="image name")


class AsicDbValidator(DVSDatabase):
    def __init__(self, db_id: str, connector: str):
        DVSDatabase.__init__(self, db_id, connector)

        # Get default .1Q Vlan ID
        self.default_vlan_id = self.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", 1)[0]

        # Build port OID to front port name mapping
        self.portoidmap = {}
        self.portnamemap = {}
        self.hostifoidmap = {}
        self.hostifnamemap = {}

        keys = self.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF", NUM_PORTS)
        for k in keys:
            fvs = self.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF", k)
            port_oid = fvs.get("SAI_HOSTIF_ATTR_OBJ_ID")
            port_name = fvs.get("SAI_HOSTIF_ATTR_NAME")

            self.portoidmap[port_oid] = port_name
            self.portnamemap[port_name] = port_oid
            self.hostifoidmap[k] = port_name
            self.hostifnamemap[port_name] = k

        # Get default ACL table and ACL rules
        self.default_acl_tables = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        assert len(self.default_acl_tables) >= 0

        self.default_acl_entries = self.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 0)


class ApplDbValidator(object):
    def __init__(self, dvs):
        self.appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.neighTbl = swsscommon.Table(self.appl_db, "NEIGH_TABLE")

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
            self.killall_processes()
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

    def killall_processes(self):
        pids = subprocess.check_output("ip netns pids %s" % (self.nsname), shell=True).decode('utf-8')
        if pids:
            for pid in pids.split('\n'):
                if len(pid) > 0:
                    os.system("kill %s" % int(pid))

    def destroy(self):
        if self.cleanup:
            self.killall_processes()
            ensure_system("ip netns delete %s" % self.nsname)

    def runcmd(self, cmd):
        try:
            out = subprocess.check_output("ip netns exec %s %s" % (self.nsname, cmd), stderr=subprocess.STDOUT, shell=True)
        except subprocess.CalledProcessError as e:
            print("------rc={} for cmd: {}------".format(e.returncode, e.cmd))
            print(e.output.rstrip())
            print("------")
            return e.returncode
        return 0

    def runcmd_async(self, cmd):
        return subprocess.Popen("ip netns exec %s %s" % (self.nsname, cmd), shell=True)

    def runcmd_output(self, cmd):
        return subprocess.check_output("ip netns exec %s %s" % (self.nsname, cmd), shell=True).decode('utf-8')

class DockerVirtualSwitch(object):
    APP_DB_ID = 0
    ASIC_DB_ID = 1
    COUNTERS_DB_ID = 2
    CONFIG_DB_ID = 4
    FLEX_COUNTER_DB_ID = 5
    STATE_DB_ID = 6

    def __init__(self, name=None, imgname=None, keeptb=False, fakeplatform=None):
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
        self.rtd   = ['fpmsyncd', 'zebra', 'staticd']
        self.teamd = ['teamsyncd', 'teammgrd']
        self.natd = ['natsyncd', 'natmgrd']
        self.alld  = self.basicd + self.swssd + self.syncd + self.rtd + self.teamd + self.natd
        self.client = docker.from_env()
        self.appldb = None

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
                    if sys.version_info < (3, 0):
                        (status, output) = commands.getstatusoutput("docker inspect --format '{{.HostConfig.NetworkMode}}' %s" % name)
                    else:
                        (status, output) = subprocess.getstatusoutput("docker inspect --format '{{.HostConfig.NetworkMode}}' %s" % name)
                    ctn_sw_id = output.split(':')[1]
                    self.cleanup = False
            if self.ctn == None:
                raise NameError("cannot find container %s" % name)

            # get base container
            for ctn in self.client.containers.list():
                if ctn.id == ctn_sw_id or ctn.name == ctn_sw_id:
                    ctn_sw_name = ctn.name

            if sys.version_info < (3, 0):
                (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % ctn_sw_name)
            else:
                (status, output) = subprocess.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % ctn_sw_name)
            self.ctn_sw_pid = int(output)

            # create virtual servers
            self.servers = []
            for i in range(NUM_PORTS):
                server = VirtualServer(ctn_sw_name, self.ctn_sw_pid, i)
                self.servers.append(server)

            self.mount = "/var/run/redis-vs/{}".format(ctn_sw_name)

            self.net_cleanup()
            self.ctn_restart()
        else:
            self.ctn_sw = self.client.containers.run('debian:jessie', privileged=True, detach=True,
                    command="bash", stdin_open=True)
            if sys.version_info < (3, 0):
                (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % self.ctn_sw.name)
            else:
                (status, output) = subprocess.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % self.ctn_sw.name)
            self.ctn_sw_pid = int(output)

            # create virtual server
            self.servers = []
            for i in range(NUM_PORTS):
                server = VirtualServer(self.ctn_sw.name, self.ctn_sw_pid, i)
                self.servers.append(server)

            # mount redis to base to unique directory
            self.mount = "/var/run/redis-vs/{}".format(self.ctn_sw.name)
            ensure_system("mkdir -p {}".format(self.mount))

            self.environment = ["fake_platform={}".format(fakeplatform)] if fakeplatform else []

            # create virtual switch container
            self.ctn = self.client.containers.run(imgname, privileged=True, detach=True,
                    environment=self.environment,
                    network_mode="container:%s" % self.ctn_sw.name,
                    volumes={ self.mount: { 'bind': '/var/run/redis', 'mode': 'rw' } })

        self.redis_sock = self.mount + '/' + "redis.sock"

        # DB wrappers are declared here, lazy-loaded in the tests
        self.app_db = None
        self.asic_db = None
        self.counters_db = None
        self.config_db = None
        self.flex_db = None
        self.state_db = None

        # Make sure everything is up and running before turning over control to the caller
        self.check_ready_status_and_init_db()

    def destroy(self):
        if self.appldb:
            del self.appldb
        if self.cleanup:
            self.ctn.remove(force=True)
            self.ctn_sw.remove(force=True)
            os.system("rm -rf {}".format(self.mount))
            for s in self.servers:
                s.destroy()

    def check_ready_status_and_init_db(self):
        try:
            # temp fix: remove them once they are moved to vs start.sh
            self.ctn.exec_run("sysctl -w net.ipv6.conf.default.disable_ipv6=0")
            for i in range(0, 128, 4):
                self.ctn.exec_run("sysctl -w net.ipv6.conf.eth%d.disable_ipv6=1" % (i + 1))

            # Verify that all of the device services have started.
            self.check_services_ready()

            # Initialize the databases.
            self.init_asicdb_validator()
            self.appldb = ApplDbValidator(self)

            # Verify that SWSS has finished initializing.
            self.check_swss_ready()

        except Exception:
            self.get_logs()
            self.destroy()
            raise

    def check_services_ready(self, timeout=30):
        """"Check if all processes in the DVS are ready."""
        re_space = re.compile('\s+')
        process_status = {}
        ready = False
        started = 0
        while True:
            # get process status
            res = self.ctn.exec_run("supervisorctl status")
            try:
                out = res.output.decode('utf-8')
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

    def check_swss_ready(self, timeout=60):
        """Verify that SWSS is ready to receive inputs.

        Almost every part of orchagent depends on ports being created and initialized
        before they can proceed with their processing. If we start the tests after orchagent
        has started running but before it has had time to initialize all the ports, then the
        first several tests will fail.
        """
        num_ports = NUM_PORTS

        # Verify that all ports have been initialized and configured
        app_db = self.get_app_db()
        startup_polling_config = PollingConfig(2, timeout, strict=True)
        app_db.wait_for_entry("PORT_TABLE", "PortInitDone", startup_polling_config)
        app_db.wait_for_entry("PORT_TABLE", "PortConfigDone", startup_polling_config)

        # Verify that all ports have been created
        asic_db = self.get_asic_db()
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_ports + 1)  # +1 CPU Port

    def net_cleanup(self):
        """clean up network, remove extra links"""

        re_space = re.compile('\s+')

        res = self.ctn.exec_run("ip link show")
        try:
            out = res.output.decode('utf-8')
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
                    print("remove extra link {}".format(pname))
        return

    def ctn_restart(self):
        self.ctn.restart()

    def restart(self):
        if self.appldb:
            del self.appldb
        self.ctn_restart()
        self.check_ready_status_and_init_db()

    # start processes in SWSS
    def start_swss(self):
        cmd = ""
        for pname in self.swssd:
            cmd += "supervisorctl start {}; ".format(pname)
        self.runcmd(['sh', '-c', cmd])
        time.sleep(5)

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
        self.asicdb = AsicDbValidator(self.ASIC_DB_ID, self.redis_sock)

    def runcmd(self, cmd):
        res = self.ctn.exec_run(cmd)
        try:
            exitcode = res.exit_code
            out = res.output.decode('utf-8')
        except AttributeError:
            exitcode = 0
            out = res
        if exitcode != 0:
            print("-----rc={} for cmd {}-----".format(exitcode, cmd))
            print(out.rstrip())
            print("-----")

        return (exitcode, out)

    def copy_file(self, path, filename):
        tarstr = io.BytesIO()
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
        ensure_system("mkdir -p {}".format(log_dir))
        p = subprocess.Popen(["tar", "--no-same-owner", "-C", "./{}".format(log_dir), "-x"], stdin=subprocess.PIPE)
        for x in stream:
            p.stdin.write(x)
        p.stdin.close()
        p.wait()
        if p.returncode:
            raise RuntimeError("Failed to unpack the archive.")
        ensure_system("chmod a+r -R log")

    def add_log_marker(self, file=None):
        marker = "=== start marker {} ===".format(datetime.now().isoformat())

        if file:
            self.runcmd(['sh', '-c', "echo \"{}\" >> {}".format(marker, file)])
        else:
            self.ctn.exec_run("logger {}".format(marker))

        return marker

    def SubscribeAppDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.APPL_DB,
                        encoding="utf-8", decode_responses=True)
        pubsub = r.pubsub()
        pubsub.psubscribe("__keyspace@0__:%s*" % objpfx)
        return pubsub

    def SubscribeAsicDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB,
                        encoding="utf-8", decode_responses=True)
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
                print(message)
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
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.APPL_DB,
                        encoding="utf-8", decode_responses=True)

        addobjs = []
        delobjs = []
        idle = 0
        prev_key = None

        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                print(message)
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
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB,
                        encoding="utf-8", decode_responses=True)

        addobjs = []
        delobjs = []
        idle = 0

        while True and idle < timeout:
            message = pubsub.get_message()
            if message:
                print(message)
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
        r = redis.Redis(unix_socket_path=self.redis_sock, encoding="utf-8",
                        decode_responses=True)
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

    def get_vlan_oid(self, asic_db, vlan_id):
        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        keys = tbl.getKeys()

        for key in keys:
            status, fvs = tbl.get(key)
            assert status, "Error reading from table %s" % "ASIC_STATE:SAI_OBJECT_TYPE_VLAN"

            for k, v in fvs:
                if k == "SAI_VLAN_ATTR_VLAN_ID" and v == vlan_id:
                    return True, key

        return False, "Not found vlan id %s" % vlan_id

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

            key_found = True

            for k, v in key_values:
                if k not in d_key or v != d_key[k]:
                    key_found = False
                    break

            if not key_found:
                continue

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
        tbl._del(interface);
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

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    def add_route(self, prefix, nexthop):
        self.runcmd("ip route add " + prefix + " via " + nexthop)
        time.sleep(1)

    def remove_route(self, prefix):
        self.runcmd("ip route del " + prefix)
        time.sleep(1)

    def create_fdb(self, vlan, mac, interface):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        fvs = swsscommon.FieldValuePairs([("port", interface),
                                          ("type", "dynamic")])
        tbl.set("Vlan" + vlan + ":" + mac, fvs)
        time.sleep(1)

    def remove_fdb(self, vlan, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        tbl._del("Vlan" + vlan + ":" + mac)
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
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB,
                        encoding="utf-8", decode_responses=True)
        swRid = r.hget("VIDTORID", swVid)

        assert swRid is not None

        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "true", fvp)
        fvp = swsscommon.FieldValuePairs([(attr, val)])
        key = "SAI_OBJECT_TYPE_SWITCH:" + swRid

        # explicit convert unicode string to str for python2
        ntf.send("set_ro", str(key), fvp)

    def get_app_db(self):
        if not self.app_db:
            self.app_db = DVSDatabase(self.APP_DB_ID, self.redis_sock)

        return self.app_db

    # FIXME: Now that AsicDbValidator is using DVSDatabase we should converge this with
    # that implementation. Save it for a follow-up PR.
    def get_asic_db(self):
        if not self.asic_db:
            db = DVSDatabase(self.ASIC_DB_ID, self.redis_sock)
            db.default_acl_tables = self.asicdb.default_acl_tables
            db.default_acl_entries = self.asicdb.default_acl_entries
            db.port_name_map = self.asicdb.portnamemap
            db.default_vlan_id = self.asicdb.default_vlan_id
            db.port_to_id_map = self.asicdb.portoidmap
            db.hostif_name_map = self.asicdb.hostifnamemap
            self.asic_db = db

        return self.asic_db

    def get_counters_db(self):
        if not self.counters_db:
            self.counters_db = DVSDatabase(self.COUNTERS_DB_ID, self.redis_sock)

        return self.counters_db

    def get_config_db(self):
        if not self.config_db:
            self.config_db = DVSDatabase(self.CONFIG_DB_ID, self.redis_sock)

        return self.config_db

    def get_flex_db(self):
        if not self.flex_db:
            self.flex_db = DVSDatabase(self.FLEX_COUNTER_DB_ID, self.redis_sock)

        return self.flex_db

    def get_state_db(self):
        if not self.state_db:
            self.state_db = DVSDatabase(self.STATE_DB_ID, self.redis_sock)

        return self.state_db

@pytest.yield_fixture(scope="module")
def dvs(request) -> DockerVirtualSwitch:
    name = request.config.getoption("--dvsname")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    fakeplatform = getattr(request.module, "DVS_FAKE_PLATFORM", None)
    dvs = DockerVirtualSwitch(name, imgname, keeptb, fakeplatform)
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


################# DVSLIB module manager fixtures #############################
@pytest.fixture(scope="class")
def dvs_acl(request, dvs) -> DVSAcl:
    return DVSAcl(dvs.get_asic_db(),
                  dvs.get_config_db(),
                  dvs.get_state_db(),
                  dvs.get_counters_db())

@pytest.yield_fixture(scope="class")
def dvs_lag_manager(request, dvs):
    request.cls.dvs_lag = dvs_lag.DVSLag(dvs.get_asic_db(),
                                         dvs.get_config_db())

@pytest.yield_fixture(scope="class")
def dvs_vlan_manager(request, dvs):
    request.cls.dvs_vlan = dvs_vlan.DVSVlan(dvs.get_asic_db(),
                                            dvs.get_config_db(),
                                            dvs.get_state_db(),
                                            dvs.get_counters_db(),
                                            dvs.get_app_db())
@pytest.yield_fixture(scope="class")
def dvs_mirror_manager(request, dvs):
    request.cls.dvs_mirror = dvs_mirror.DVSMirror(dvs.get_asic_db(),
                                            dvs.get_config_db(),
                                            dvs.get_state_db(),
                                            dvs.get_counters_db(),
                                            dvs.get_app_db())
@pytest.yield_fixture(scope="class")
def dvs_policer_manager(request, dvs):
    request.cls.dvs_policer = dvs_policer.DVSPolicer(dvs.get_asic_db(),
                                            dvs.get_config_db())
##################### DPB fixtures ###########################################
def create_dpb_config_file(dvs):
    cmd = "sonic-cfggen -j /etc/sonic/init_cfg.json -j /tmp/ports.json --print-data > /tmp/dpb_config_db.json"
    dvs.runcmd(['sh', '-c', cmd])
    cmd = "mv /etc/sonic/config_db.json /etc/sonic/config_db.json.bak"
    dvs.runcmd(cmd)
    cmd = "cp /tmp/dpb_config_db.json /etc/sonic/config_db.json"
    dvs.runcmd(cmd)

def remove_dpb_config_file(dvs):
    cmd = "mv /etc/sonic/config_db.json.bak /etc/sonic/config_db.json"
    dvs.runcmd(cmd)

@pytest.yield_fixture(scope="module")
def dpb_setup_fixture(dvs):
    create_dpb_config_file(dvs)
    dvs.restart()
    yield
    remove_dpb_config_file(dvs)
