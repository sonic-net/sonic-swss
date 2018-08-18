import os
import re
import time
import docker
import pytest
import commands
from swsscommon import swsscommon

def pytest_addoption(parser):
    parser.addoption("--dvsname", action="store", default=None,
                      help="dvs name")

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

class VirtualServer(object):
    def __init__(self, ctn_name, pid, i):
        self.nsname = "%s-srv%d" % (ctn_name, i)
        self.vifname = "vEthernet%d" % (i * 4)
        self.cleanup = True

        # create netns
        if os.path.exists("/var/run/netns/%s" % self.nsname):
            self.cleanup = False
        else:
            os.system("ip netns add %s" % self.nsname)

            # create vpeer link
            os.system("ip link add %s type veth peer name %s" % (self.nsname[0:12], self.vifname))
            os.system("ip link set %s netns %s" % (self.nsname[0:12], self.nsname))
            os.system("ip link set %s netns %d" % (self.vifname, pid))

            # bring up link in the virtual server
            os.system("ip netns exec %s ip link set dev %s name eth0" % (self.nsname, self.nsname[0:12]))
            os.system("ip netns exec %s ip link set dev eth0 up" % (self.nsname))

            # bring up link in the virtual switch
            os.system("nsenter -t %d -n ip link set dev %s up" % (pid, self.vifname))

    def __del__(self):
        if self.cleanup:
            os.system("ip netns delete %s" % self.nsname)

    def runcmd(self, cmd):
        os.system("ip netns exec %s %s" % (self.nsname, cmd))

class DockerVirtualSwitch(object):
    def __init__(self, name=None):
        self.pnames = ['fpmsyncd',
                       'intfmgrd',
                       'intfsyncd',
                       'neighsyncd',
                       'orchagent',
                       'portsyncd',
                       'redis-server',
                       'rsyslogd',
                       'syncd',
                       'teamsyncd',
                       'vlanmgrd',
                       'zebra']
        self.mount = "/var/run/redis-vs"
        self.redis_sock = self.mount + '/' + "redis.sock"
        self.client = docker.from_env()

        self.ctn = None
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

            # create virtual switch container
            self.ctn = self.client.containers.run('docker-sonic-vs', privileged=True, detach=True,
                    network_mode="container:%s" % self.ctn_sw.name,
                    volumes={ self.mount: { 'bind': '/var/run/redis', 'mode': 'rw' } })

        self.check_ready()
        self.init_asicdb_validator()

    def destroy(self):
        if self.cleanup:
            self.ctn.remove(force=True)
            self.ctn_sw.remove(force=True)
            for s in self.servers:
                del(s)

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
            for pname in self.pnames:
                try:
                    if process_status[pname] != "RUNNING":
                        ready = False
                except KeyError:
                    ready = False

            if ready == True:
                break

            started += 1
            if started > timeout:
                raise ValueError(out)

            time.sleep(1)

    def restart(self):
        self.ctn.restart()

    def init_asicdb_validator(self):
        self.asicdb = AsicDbValidator(self)

    def runcmd(self, cmd):
        return self.ctn.exec_run(cmd)

@pytest.yield_fixture(scope="module")
def dvs(request):
    name = request.config.getoption("--dvsname")
    dvs = DockerVirtualSwitch(name)
    yield dvs
    dvs.destroy()
