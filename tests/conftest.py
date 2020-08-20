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
import random
import string
import subprocess
import sys
if sys.version_info < (3, 0):
    import commands

from datetime import datetime
from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig, wait_for_result
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
    parser.addoption("--vctns", action="store", default=None,
                     help="vct namespace")
    parser.addoption("--topo", action="store", default=None,
                     help="vct topology file")
    parser.addoption("--forcedvs", action="store_true", default=False,
                      help="force persistent dvs when ports < 32")
    parser.addoption("--keeptb", action="store_true", default=False,
                      help="keep testbed after test")
    parser.addoption("--imgname", action="store", default="docker-sonic-vs",
                      help="image name")
    parser.addoption("--max_cpu",
                     action="store",
                     default=2,
                     type=int,
                     help="Max number of CPU cores to use, if available. (default = 2)")

def random_string(size=4, chars=string.ascii_uppercase + string.digits):
   return ''.join(random.choice(chars) for x in range(size))

class AsicDbValidator(DVSDatabase):
    def __init__(self, db_id: int, connector: str):
        DVSDatabase.__init__(self, db_id, connector)
        self._wait_for_asic_db_to_initialize()
        self._populate_default_asic_db_values()
        self._generate_oid_to_interface_mapping()

    def _wait_for_asic_db_to_initialize(self) -> None:
        """Wait up to 30 seconds for the default fields to appear in ASIC DB."""
        def _verify_db_contents():
            # We expect only the default VLAN
            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN")) != 1:
                return (False, None)

            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")) != NUM_PORTS:
                return (False, None)

            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")) != 0:
                return (False, None)

            return (True, None)

        # Verify that ASIC DB has been fully initialized
        init_polling_config = PollingConfig(2, 30, strict=True)
        wait_for_result(_verify_db_contents, init_polling_config)

    def _generate_oid_to_interface_mapping(self) -> None:
        """Generate the OID->Name mappings for ports and host interfaces."""
        self.portoidmap = {}
        self.portnamemap = {}
        self.hostifoidmap = {}
        self.hostifnamemap = {}

        host_intfs = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        for intf in host_intfs:
            fvs = self.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF", intf)
            port_oid = fvs.get("SAI_HOSTIF_ATTR_OBJ_ID")
            port_name = fvs.get("SAI_HOSTIF_ATTR_NAME")

            self.portoidmap[port_oid] = port_name
            self.portnamemap[port_name] = port_oid
            self.hostifoidmap[intf] = port_name
            self.hostifnamemap[port_name] = intf

    def _populate_default_asic_db_values(self) -> None:
        # Get default .1Q Vlan ID
        self.default_vlan_id = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN")[0]

        self.default_acl_tables = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        self.default_acl_entries = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")


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

    def __init__(
            self,
            name=None,
            imgname=None,
            keeptb=False,
            fakeplatform=None,
            log_path=None,
            max_cpu=2,
            forcedvs=None,
            vct=None,
            newctnname=None,
            ctnmounts=None
    ):
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
        self.dvsname = name
        self.vct = vct

        if subprocess.check_call(["/sbin/modprobe", "team"]) != 0:
            raise NameError("cannot install kernel team module")

        self.log_path = log_path

        self.ctn = None
        if keeptb:
            self.cleanup = False
        else:
            self.cleanup = True
        ctn_sw_id = -1
        ctn_sw_name = None
        self.persistent = False
        if name != None:
            # get virtual switch container
            for ctn in self.client.containers.list():
                if ctn.name == name:
                    self.ctn = ctn
                    if sys.version_info < (3, 0):
                        (status, output) = commands.getstatusoutput("docker inspect --format '{{.HostConfig.NetworkMode}}' %s" % name)
                    else:
                        (status, output) = subprocess.getstatusoutput("docker inspect --format '{{.HostConfig.NetworkMode}}' %s" % name)
                    if output != 'none':
                        ctn_sw_id = output.split(':')[1]
                    # Persistent DVS is available.
                    self.cleanup = False
                    self.persistent = True
            if self.ctn == None:
                raise NameError("cannot find container %s" % name)

            self.num_net_interfaces = self.net_interface_count()

            if self.num_net_interfaces > NUM_PORTS:
                raise ValueError("persistent dvs is not valid for testbed with ports > %d" % NUM_PORTS)

            if self.num_net_interfaces < NUM_PORTS and not forcedvs:
                raise ValueError("persistent dvs does not have %d ports needed by testbed" % NUM_PORTS)

            # get base container
            for ctn in self.client.containers.list():
                if ctn.id == ctn_sw_id or ctn.name == ctn_sw_id:
                    ctn_sw_name = ctn.name

            if ctn_sw_name:
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
            else:
                self.mount = "/var/run/redis-vs/{}".format(name)
            self.net_cleanup()
            # As part of https://github.com/Azure/sonic-buildimage/pull/4499
            # VS support dynamically create Front-panel ports so save the orginal
            # config db for persistent DVS
            self.runcmd("mv /etc/sonic/config_db.json /etc/sonic/config_db.json.orig")
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

            if self.vct is not None:
                self.vct_connect(newctnname)

            # mount redis to base to unique directory
            self.mount = "/var/run/redis-vs/{}".format(self.ctn_sw.name)
            ensure_system("mkdir -p {}".format(self.mount))

            self.environment = ["fake_platform={}".format(fakeplatform)] if fakeplatform else []
            kwargs = {}
            if newctnname:
               kwargs['name'] = newctnname
               self.dvsname = newctnname
            vols = { self.mount: { 'bind': '/var/run/redis', 'mode': 'rw' } }
            if ctnmounts is not None:
                for k,v in ctnmounts.items():
                    vols[k] = v
            kwargs['volumes'] = vols
            # create virtual switch container
            self.ctn = self.client.containers.run(imgname,
                                                  privileged=True,
                                                  detach=True,
                                                  environment=self.environment,
                                                  network_mode=f"container:{self.ctn_sw.name}",
                                                  cpu_count=max_cpu,
                                                  **kwargs)
        if sys.version_info < (3, 0):
            _, output = commands.getstatusoutput("docker inspect --format"
                                              " '{{.State.Pid}}' %s" % self.ctn.name)
        else:
            _, output = subprocess.getstatusoutput("docker inspect --format"
                                              " '{{.State.Pid}}' %s" % self.ctn.name)
        self.pid = int(output)
        self.redis_sock = self.mount + '/' + "redis.sock"
        self.redis_chassis_sock = self.mount + "/redis_chassis.sock"

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
        # In case persistent dvs was used removed all the extra server link
        # that were created
        if self.persistent:
            for s in self.servers:
                s.destroy()
        # persistent and clean-up flag are mutually exclusive
        elif self.cleanup:
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
        """Check if all processes in the DVS are ready."""
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

    def check_swss_ready(self, timeout=300):
        """Verify that SWSS is ready to receive inputs.

        Almost every part of orchagent depends on ports being created and initialized
        before they can proceed with their processing. If we start the tests after orchagent
        has started running but before it has had time to initialize all the ports, then the
        first several tests will fail.
        """
        num_ports = NUM_PORTS

        # Verify that all ports have been initialized and configured
        app_db = self.get_app_db()
        startup_polling_config = PollingConfig(5, timeout, strict=True)

        def _polling_function():
            port_table_keys = app_db.get_keys("PORT_TABLE")
            return ("PortInitDone" in port_table_keys and "PortConfigDone" in port_table_keys, None)

        wait_for_result(_polling_function, startup_polling_config)

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
                m = re.compile("(eth|lo|Bridge|Ethernet|vlan|inband)").match(pname)
                if not m:
                    self.ctn.exec_run("ip link del {}".format(pname))
                    print("remove extra link {}".format(pname))
        return

    def net_interface_count(self):
        """get the interface count in persistent DVS Container
           if not found or some error then return 0 as default"""

        res = self.ctn.exec_run(['sh', '-c', 'ip link show | grep -oE eth[0-9]+ | grep -vc eth0'])
        if not res.exit_code:
            try:
                out = res.output.decode('utf-8')
            except AttributeError:
                return 0
            return int(out.rstrip('\n'))
        else:
            return 0
      
    def vct_connect(self, ctnname):
        data = self.vct.get_inband(ctnname)
        if "inband_address" in data:
            ifpair = data["inband_intf_pair"]
            ifname = data["inband_intf"]
            iaddr = data["inband_address"]
            self.vct.connect(ifname, ifpair, str(self.ctn_sw_pid))
            self.ctn_sw.exec_run("ip link set dev " + ifpair + " up")
            self.ctn_sw.exec_run("ip link add link " + ifpair +
                                 " name vlan4094 type vlan id 4094")
            self.ctn_sw.exec_run("ip addr add " + iaddr + " dev vlan4094")
            self.ctn_sw.exec_run("ip link set dev vlan4094 up")

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

    def get_logs(self):
        log_dir = os.path.join("log", self.log_path) if self.log_path else "log"

        ensure_system(f"rm -rf {log_dir}")
        ensure_system(f"mkdir -p {log_dir}")

        p = subprocess.Popen(["tar", "--no-same-owner", "-C", os.path.join("./", log_dir), "-x"], stdin=subprocess.PIPE)

        stream, _ = self.ctn.get_archive("/var/log/")
        for x in stream:
            p.stdin.write(x)
        p.stdin.close()
        p.wait()

        if p.returncode:
            raise RuntimeError("Failed to unpack the log archive.")

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
    
    def change_route(self, prefix, nexthop):
        self.runcmd("ip route change " + prefix + " via " + nexthop)
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

class DockerVirtualChassisTopology(object):
    def __init__(
            self, 
            namespace=None, 
            imgname=None, 
            keeptb=False,
            fakeplatform=None,
            log_path=None,
            max_cpu=2,
            forcedvs=None,
            topoFile=None ):
        self.ns = namespace
        self.chassbr = "br4chs"
        self.keeptb = keeptb
        self.fakeplatform = fakeplatform
        self.topoFile = topoFile
        self.imgname = imgname
        self.ctninfo = {}
        self.dvss = {}
        self.inbands = {}
        self.log_path = log_path
        self.max_cpu = max_cpu
        self.forcedvs = forcedvs
        if self.ns is None:
           self.ns = random_string()
        print("VCT ns: " + self.ns)
        self.find_all_ctns()
        with open(self.topoFile, "r") as f:
            self.virt_topo = json.load(f)["VIRTUAL_TOPOLOGY"]
        self.oper = "create"
        self.handle_request()

    def runcmd(self, cmd, addns=True):
        try:
            netns = ""
            if addns:
               netns = "sudo ip netns exec %s" % self.ns
            subprocess.check_output("%s %s" % (netns, cmd),
                                    stderr=subprocess.STDOUT, shell=True)
        except subprocess.CalledProcessError as e:
            print("------rc={} for cmd: {}------".format(e.returncode, e.cmd))
            print(e.output.rstrip())
            print("------")
            return e.returncode
        return 0

    def connect(self, ifname, ifpair, pid):
        self.runcmd("ip link del " + ifname)
        self.runcmd("ip link add " + ifname + " type veth peer name " + ifpair)
        self.runcmd("ip link set " + ifpair + " netns " + pid)
        self.runcmd("ip link set dev " + ifname + " up")
        self.runcmd("brctl addif " + self.chassbr + " " + ifname)

    def connect_ethintfs(self, intfs, nbrConns, pid, ctnname):
        for intf in intfs:
            ifn = ctnname[:9] + "." + intf
            self.runcmd("ip link add " + ifn + " type veth peer name " +  intf)
            self.runcmd("ip link set " + intf + " netns " + pid)
            self.runcmd("ip link set dev " + ifn + " up")
        for intf in nbrConns:
            br = nbrConns[intf]
            if br != "":
                self.runcmd(" brctl addif " + br + " " + intf)

    def find_all_ctns(self):
        suffix = "." + self.ns
        for ctn in docker.from_env().containers.list():
            if ctn.name.endswith(suffix):
                self.dvss[ctn.name] = DockerVirtualSwitch(ctn.name, self.imgname, self.keeptb,
                                                          self.fakeplatform, log_path=ctn.name,
                                                          max_cpu=self.max_cpu, forcedvs=self.forcedvs,
                                                          vct=self)
        if self.chassbr is None and len(self.dvss) > 0:
            ret, res = self.ctn_runcmd(self.dvss.values()[0].ctn,
                        "sonic-cfggen --print-data -j /usr/share/sonic/virtual_chassis/vct_connections.json")
            if ret == 0:
                out = json.loads(res)
                self.chassbr = out["chassis_bridge"]

    def get_ctn(self, ctnname):
        if ctnname in self.dvss:
           return self.dvss[ctnname].ctn
        return None

    def ctn_runcmd(self, ctn, cmd):
        res = ctn.exec_run(cmd)
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

    def set_ctninfo(self, ctn, name, pid):
        self.ctninfo[ctn] = [name, pid]

    def get_ctninfo(self, ctn):
        res = self.ctninfo[ctn]
        return res[0], res[1]

    def runcmd_on_ctn(self, ctnname, cmd):
        ctn = self.get_ctn(ctnname)
        return self.ctn_runcmd(ctn, cmd)

    def handle_request(self):
        if self.oper == "verify":
            self.verify_vct()
            return
        ctn = self.virt_topo["chassis_instances"]
        # When virtual chassis is created,
        # 1. new namespace and bridge for the chassis are created first
        # 2. containers for each vs instance need to be created
        # 3. neighbor connections are setup at last.
        # when the virtual chassis is deleted,
        # 1. neighbors are deleted
        # 2. containers are deleted
        # 3. namespace and chassis bridge are deleted
        if self.oper == "create":
            self.runcmd("sudo ip netns add " + self.ns, addns=False)
            self.handle_bridge(self.chassbr)

            for ctndir in ctn:
                self.create_vct_ctn(ctndir)
            if "neighbor_connections" in self.virt_topo:
                self.handle_neighconn()
            retry = 0
            while self.verify_vct() is False and retry < 10:
                print("wait for chassis to be ready")
                time.sleep(1)
                retry += 1
        if self.oper == "delete":
            for dv in self.dvss.values():
                dv.destroy()
            self.handle_bridge(self.chassbr)
            self.runcmd("sudo ip netns del " + self.ns, addns=False)

    def destroy(self):
        self.verify_vct()
        if self.keeptb:
            return
        self.oper = "delete"
        self.handle_request()

    def restart(self):
        for dv in self.dvss.values():
            dv.restart()

    def get_logs(self, name):
        for dv in self.dvss.values():
            if not dv.dvsname:
                dv.get_logs(name)
            else:
                dv.get_logs()

    def handle_bridge(self, brName):
        if self.oper == "create":
            self.runcmd(" brctl addbr " + brName)
            self.runcmd(" ip link set dev " + brName + " up")
        else:
            self.runcmd(" ip link set dev " + brName + " down")
            self.runcmd(" brctl delbr " + brName)
        return

    def create_vct_ctn(self, ctndir):
        cwd = os.getcwd()
        chassis_config_dir = cwd + "/virtual_chassis/" + ctndir
        chassis_config_file =  chassis_config_dir + "/default_config.json"
        with open(chassis_config_file, "r") as cfg:
            defcfg = json.load(cfg)["DEVICE_METADATA"]["localhost"]
            ctnname = defcfg["hostname"] + "." + self.ns
            vol = {}
            vol[chassis_config_dir] = {"bind": "/usr/share/sonic/virtual_chassis", "mode": "ro"}

            # pass self.ns into the vs to be use for vs restarts by swss conftest.
            # connection to chassbr is setup by chassis_connect.py within the vs
            data = {}
            if "inband_address" in defcfg.keys():
                data["inband_intf"] = self.ns + "veth" + ctndir
                data["inband_intf_pair"] = "inband"
                data["inband_address"] = defcfg["inband_address"]
            self.inbands[ctnname] = data
            if ctnname not in self.dvss:
                self.dvss[ctnname] = DockerVirtualSwitch(name=None, imgname=self.imgname,
                                                         keeptb=self.keeptb,
                                                         fakeplatform=self.fakeplatform,
                                                         log_path=self.log_path,
                                                         max_cpu=self.max_cpu,
                                                         forcedvs=self.forcedvs,
                                                         vct=self,newctnname=ctnname,
                                                         ctnmounts=vol)
            self.set_ctninfo(ctndir, ctnname, self.dvss[ctnname].pid)
        return

    def get_inband(self, ctnname):
        if ctnname in self.inbands:
            return self.inbands[ctnname]
        return {}

    def get_topo_neigh(self):
        instance_to_neighbor_map = {}
        if "neighbor_connections" not in self.virt_topo:
            return instance_to_neighbor_map

        working_dir = os.getcwd()
        for conn, endpoints in self.virt_topo["neighbor_connections"].items():
            chassis_instance = conn.split('-')[0]
            neighbor_instance = conn.split('-')[1]

            chassis_config_dir = os.path.join(working_dir, "virtual_chassis", chassis_instance)
            chassis_config_file = os.path.join(chassis_config_dir, "default_config.json")
            chassis_container_name = ""
            with open(chassis_config_file, "r") as cfg:
                device_info = json.load(cfg)["DEVICE_METADATA"]["localhost"]
                chassis_container_name = device_info["hostname"] + "." + self.ns

            neighbor_veth_intf = int(endpoints[neighbor_instance].split("eth")[1])
            neighbor_host_intf = f"Ethernet{(neighbor_veth_intf - 1) * 4}"

            chassis_veth_intf = int(endpoints[chassis_instance].split("eth")[1])
            chassis_host_intf = f"Ethernet{(chassis_veth_intf - 1) * 4}"

            neighbor_config_file = os.path.join(working_dir, "virtual_chassis", neighbor_instance, "default_config.json")
            with open(neighbor_config_file, "r") as cfg:
                intf_config = json.load(cfg)["INTERFACE"]
                for key in intf_config:
                    neighbor_address = ""
                    if key.lower().startswith(f"{neighbor_host_intf}|"):
                        host_intf_address = re.split("/|\\|", key)

                        if len(host_intf_address) > 1:
                            neighbor_address = host_intf_address[1]

                        if neighbor_address == "":
                            continue

                        if chassis_container_name not in instance_to_neighbor_map:
                            instance_to_neighbor_map[chassis_container_name] = []

                        instance_to_neighbor_map[chassis_container_name].append((chassis_veth_intf - 1,
                                                                                 neighbor_address,
                                                                                 chassis_host_intf))

        return instance_to_neighbor_map

    def handle_neighconn(self):
        if self.oper != "create":
            return

        instance_to_neighbor_map = self.get_topo_neigh()
        for ctnname, nbraddrs in instance_to_neighbor_map.items():
            if ctnname not in self.dvss:
                continue

            for server, neighbor_address, host_intf in nbraddrs:
                self.dvss[ctnname].servers[server].runcmd("ifconfig eth0 down")
                self.dvss[ctnname].servers[server].runcmd("ifconfig eth0 up")
                self.dvss[ctnname].servers[server].runcmd(f"ifconfig eth0 {neighbor_address}")
                self.dvss[ctnname].runcmd(f"config interface startup {host_intf}")

    def verify_conns(self):
        passed = True
        if "neighbor_connections" not in self.virt_topo:
            return passed
        instance_to_neighbor_map = self.get_topo_neigh()
        for ctnname, nbraddrs in instance_to_neighbor_map.items():
            for item in nbraddrs:
                nbraddr = item[1]
                print("verify neighbor connectivity from %s to %s nbrAddr " % (
                   ctnname, nbraddr))
                _, out = self.runcmd_on_ctn(ctnname, " ping -c 5 " + nbraddr)
                if "5 received" not in out.split("\n")[-3]:
                    print("FAILED:%s: ping %s \n res: %s " % (ctnname, nbraddr, out))
                    passed = False
        return passed

    def verify_crashes(self):
        ctn = self.virt_topo['chassis_instances']
        passed = True
        # to avoid looking at crashes from previous runs,
        # ignore the crashes check when testbed is preserved
        if self.keeptb:
            return passed
        # verify no crashes
        for ctndir in ctn:
            ctnname, _ = self.get_ctninfo(ctndir)
            res, out = self.runcmd_on_ctn(ctnname,
                                 " grep 'terminated by SIGABRT' /var/log/syslog ")
            if out != "":
                print("FAILED: container %s has agent termination(s)" % ctnname)
                print(res, out)
                passed = False
        return passed

    def verify_vct(self):
        ret1 = self.verify_conns()
        ret2 = self.verify_crashes()
        print("vct verifications passed ? %s" % (ret1 and ret2))
        return ret1 and ret2

@pytest.yield_fixture(scope="module")
def dvs(request) -> DockerVirtualSwitch:
    name = request.config.getoption("--dvsname")
    forcedvs = request.config.getoption("--forcedvs")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    max_cpu = request.config.getoption("--max_cpu")
    fakeplatform = getattr(request.module, "DVS_FAKE_PLATFORM", None)
    log_path = name if name else request.module.__name__

    dvs = DockerVirtualSwitch(name, imgname, keeptb, fakeplatform, log_path, max_cpu, forcedvs)

    yield dvs

    dvs.get_logs()
    dvs.destroy()
    # restore original config db
    if dvs.persistent:
        dvs.runcmd("mv /etc/sonic/config_db.json.orig /etc/sonic/config_db.json")
        dvs.ctn_restart()

@pytest.yield_fixture(scope="module")
def vct(request):
    vctns = request.config.getoption("--vctns")
    topo = request.config.getoption("--topo")
    forcedvs = request.config.getoption("--forcedvs")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    max_cpu = request.config.getoption("--max_cpu")
    log_path = vctns if vctns else request.module.__name__
    fakeplatform = getattr(request.module, "DVS_FAKE_PLATFORM", None)
    if not topo:
        # use ecmp topology as default
        topo = "virtual_chassis/chassis_with_ecmp_neighbors.json"
    vct = DockerVirtualChassisTopology(vctns, imgname, keeptb, fakeplatform, log_path, max_cpu,
                                       forcedvs, topo)
    yield vct
    vct.get_logs(request.module.__name__)
    vct.destroy()

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
    if dvs.vct is None:
        dvs.restart()
    else:
        dvs.vct.restart()
    yield
    remove_dpb_config_file(dvs)
