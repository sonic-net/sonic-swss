import os
import re
import time
import docker
import pytest
import commands

def pytest_addoption(parser):
    parser.addoption("--dvsname", action="store", default=None,
                      help="dvs name")

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
                    cnt_sw_id = output.split(':')[1]
                    self.cleanup = False
            if self.ctn == None:
                raise NameError("cannot find container %s" % name)

            # get base container
            for ctn in self.client.containers.list():
                if ctn.id == cnt_sw_id:
                    cnt_sw_name = ctn.name
           
            (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % cnt_sw_name)
            self.cnt_sw_pid = int(output)

            # create virtual servers
            self.servers = []
            for i in range(32):
                server = VirtualServer(cnt_sw_name, self.cnt_sw_pid, i)
                self.servers.append(server)
        else:
            self.ctn_sw = self.client.containers.run('debian:jessie', privileged=True, detach=True,
                    command="bash", stdin_open=True)
            (status, output) = commands.getstatusoutput("docker inspect --format '{{.State.Pid}}' %s" % self.ctn_sw.name)
            self.cnt_sw_pid = int(output)

            # create virtual server
            self.servers = []
            for i in range(32):
                server = VirtualServer(self.ctn_sw.name, self.cnt_sw_pid, i)
                self.servers.append(server)

            # create virtual switch container
            self.ctn = self.client.containers.run('docker-sonic-vs', privileged=True, detach=True,
                    network_mode="container:%s" % self.ctn_sw.name,
                    volumes={ self.mount: { 'bind': '/var/run/redis', 'mode': 'rw' } })

    def destroy(self):
        if self.cleanup:
            self.ctn.remove(force=True)
            self.ctn_sw.remove(force=True)
            for s in self.servers:
                del(s)

    def ready(self, timeout=30):
        '''check if all processes in the dvs is ready'''

        re_space = re.compile('\s+')
        process_status = {}
        ready = False
        started = 0
        while True:
            # get process status
            out = self.ctn.exec_run("supervisorctl status")
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

    def runcmd(self, cmd):
        return self.ctn.exec_run(cmd)

@pytest.yield_fixture(scope="module")
def dvs(request):
    name = request.config.getoption("--dvsname")
    dvs = DockerVirtualSwitch(name)
    yield dvs
    dvs.destroy()
