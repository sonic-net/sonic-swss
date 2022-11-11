import os
import re
import time
import json
import redis
import docker
import pytest
import random
import string
import subprocess
import sys
import tarfile
import io

from typing import Dict, Tuple
from datetime import datetime

from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig, wait_for_result
from dvslib.dvs_acl import DVSAcl
from dvslib.dvs_pbh import DVSPbh
from dvslib.dvs_route import DVSRoute
from dvslib import dvs_vlan
from dvslib import dvs_port
from dvslib import dvs_lag
from dvslib import dvs_mirror
from dvslib import dvs_policer

from buffer_model import enable_dynamic_buffer

# FIXME: For the sake of stabilizing the PR pipeline we currently assume there are 32 front-panel
# ports in the system (much like the rest of the test suite). This should be adjusted to accomodate
# a dynamic number of ports. GitHub Issue: Azure/sonic-swss#1384.
NUM_PORTS = 32

# Voq asics will have 16 fabric ports created (defined in Azure/sonic-buildimage#7629).
FABRIC_NUM_PORTS = 16

def ensure_system(cmd):
    rc, output = subprocess.getstatusoutput(cmd)
    if rc:
        raise RuntimeError(f"Failed to run command: {cmd}. rc={rc}. output: {output}")

def pytest_addoption(parser):
    parser.addoption("--dvsname",
                     action="store",
                     default=None,
                     help="Name of a persistent DVS container to run the tests with. Mutually exclusive with --force-recreate-dvs")

    parser.addoption("--forcedvs",
                     action="store_true",
                     default=False,
                     help="Force tests to run in persistent DVS containers with <32 ports")

    parser.addoption("--force-recreate-dvs",
                     action="store_true",
                     default=False,
                     help="Force the DVS container to be recreated between each test module. Mutually exclusive with --dvsname")

    parser.addoption("--keeptb",
                     action="store_true",
                     default=False,
                     help="Keep testbed running after tests for debugging purposes")

    parser.addoption("--imgname",
                     action="store",
                     default="docker-sonic-vs:latest",
                     help="Name of an image to use for the DVS container")

    parser.addoption("--max_cpu",
                     action="store",
                     default=2,
                     type=int,
                     help="Max number of CPU cores to use, if available. (default = 2)")

    parser.addoption("--vctns",
                     action="store",
                     default=None,
                     help="Namespace for the Virtual Chassis Topology")

    parser.addoption("--topo",
                     action="store",
                     default=None,
                     help="Topology file for the Virtual Chassis Topology")

    parser.addoption("--buffer_model",
                     action="store",
                     default="traditional",
                     help="Buffer model")

    parser.addoption("--graceful-stop",
                     action="store_true",
                     default=False,
                     help="Stop swss and syncd before stopping a conatainer")


def random_string(size=4, chars=string.ascii_uppercase + string.digits):
    return "".join(random.choice(chars) for x in range(size))


class AsicDbValidator(DVSDatabase):
    def __init__(self, db_id: int, connector: str, switch_type: str):
        DVSDatabase.__init__(self, db_id, connector)
        if switch_type not in ['fabric']:
           self._wait_for_asic_db_to_initialize()
           self._populate_default_asic_db_values()
           self._generate_oid_to_interface_mapping()

    def _wait_for_asic_db_to_initialize(self) -> None:
        """Wait up to 30 seconds for the default fields to appear in ASIC DB."""
        def _verify_db_contents():
            # We expect only the default VLAN
            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN")) != 1:
                return (False, None)

            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")) < NUM_PORTS:
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

        self.default_copp_policers = self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_POLICER")


class ApplDbValidator(DVSDatabase):
    NEIGH_TABLE = "NEIGH_TABLE"

    def __init__(self, db_id: int, connector: str):
        DVSDatabase.__init__(self, db_id, connector)

    def __del__(self):
        # Make sure no neighbors on physical interfaces
        neighbors = self.get_keys(self.NEIGH_TABLE)
        for neighbor in neighbors:
            m = re.match(r"eth(\d+)", neighbor)
            if not m:
                continue
            assert int(m.group(1)) > 0


class VirtualServer:
    def __init__(self, ctn_name: str, pid: int, i: int):
        self.nsname = f"{ctn_name}-srv{i}"
        self.pifname = f"eth{i + 1}"
        self.cleanup = True

        # create netns
        if os.path.exists(os.path.join("/var/run/netns/", self.nsname)):
            self.kill_all_processes()
            self.cleanup = False
        else:
            ensure_system(f"ip netns add {self.nsname}")

            # create vpeer link
            ensure_system(
                f"ip netns exec {self.nsname} ip link add {self.nsname[0:12]}"
                f" type veth peer name {self.pifname}"
            )

            # ensure self.pifname is not already an interface in the DVS net namespace
            rc, _ = subprocess.getstatusoutput(f"nsenter -t {pid} -n ip link show | grep '{self.pifname}@'")
            if not rc:
                try:
                    ensure_system(f"nsenter -t {pid} -n ip link delete {self.pifname}")
                except RuntimeError as e:
                    # Occasionally self.pifname will get deleted between us checking for its existence
                    # and us deleting it ourselves. In this case we can continue normally
                    if "cannot find device" in str(e).lower():
                        pass
                    else:
                        raise e

            ensure_system(f"ip netns exec {self.nsname} ip link set {self.pifname} netns {pid}")

            # bring up link in the virtual server
            ensure_system(f"ip netns exec {self.nsname} ip link set dev {self.nsname[0:12]} name eth0")
            ensure_system(f"ip netns exec {self.nsname} ip link set dev eth0 up")
            ensure_system(f"ip netns exec {self.nsname} ethtool -K eth0 tx off")

            # bring up link in the virtual switch
            ensure_system(f"nsenter -t {pid} -n ip link set dev {self.pifname} up")

            # disable arp, so no neigh on physical interfaces
            ensure_system(f"nsenter -t {pid} -n ip link set arp off dev {self.pifname}")
            ensure_system(f"nsenter -t {pid} -n sysctl -w net.ipv6.conf.{self.pifname}.disable_ipv6=1")

    def __repr__(self):
        return f'<VirtualServer> {self.nsname}'

    def kill_all_processes(self) -> None:
        pids = subprocess.check_output(f"ip netns pids {self.nsname}", shell=True).decode("utf-8")
        if pids:
            for pid in pids.split('\n'):
                if len(pid) > 0:
                    os.system(f"kill {pid}")

    def destroy(self) -> None:
        if self.cleanup:
            self.kill_all_processes()
            ensure_system(f"ip netns delete {self.nsname}")

    def runcmd(self, cmd: str) -> int:
        try:
            subprocess.check_output(f"ip netns exec {self.nsname} {cmd}", stderr=subprocess.STDOUT, shell=True)
        except subprocess.CalledProcessError as e:
            print(f"------rc={e.returncode} for cmd: {e.cmd}------")
            print(e.output.rstrip())
            print("------")
            return e.returncode

        return 0

    # used in buildimage tests, do not delete
    def runcmd_async(self, cmd: str) -> subprocess.Popen:
        return subprocess.Popen(f"ip netns exec {self.nsname} {cmd}", shell=True)

    def runcmd_output(self, cmd: str) -> str:
        return subprocess.check_output(f"ip netns exec {self.nsname} {cmd}", shell=True).decode("utf-8")

class DockerVirtualSwitch:
    APPL_DB_ID = 0
    ASIC_DB_ID = 1
    COUNTERS_DB_ID = 2
    CONFIG_DB_ID = 4
    FLEX_COUNTER_DB_ID = 5
    STATE_DB_ID = 6

    # FIXME: Should be broken up into helper methods in a later PR.
    def __init__(
        self,
        name: str = None,
        imgname: str = None,
        keeptb: bool = False,
        env: list = [],
        log_path: str = None,
        max_cpu: int = 2,
        forcedvs: bool = None,
        vct: str = None,
        newctnname: str = None,
        ctnmounts: Dict[str, str] = None,
        buffer_model: str = None,
    ):
        self.basicd = ["redis-server", "rsyslogd"]
        self.swssd = [
            "orchagent",
            "intfmgrd",
            "neighsyncd",
            "portsyncd",
            "vlanmgrd",
            "vrfmgrd",
            "portmgrd"
        ]
        self.syncd = ["syncd"]
        self.rtd = ["fpmsyncd", "zebra", "staticd"]
        self.teamd = ["teamsyncd", "teammgrd"]
        self.natd = ["natsyncd", "natmgrd"]
        self.alld = self.basicd + self.swssd + self.syncd + self.rtd + self.teamd + self.natd

        self.log_path = log_path
        self.dvsname = name
        self.vct = vct
        self.ctn = None

        self.cleanup = not keeptb

        ctn_sw_id = -1
        ctn_sw_name = None

        self.persistent = False

        self.client = docker.from_env()

        # Use the provided persistent DVS testbed
        if name:
            # get virtual switch container
            for ctn in self.client.containers.list():
                if ctn.name == name:
                    self.ctn = ctn
                    _, output = subprocess.getstatusoutput(f"docker inspect --format '{{{{.HostConfig.NetworkMode}}}}' {name}")
                    ctn_sw_id = output.split(':')[1]

                    # Persistent DVS is available.
                    self.cleanup = False
                    self.persistent = True

            if not self.ctn:
                raise NameError(f"cannot find container {name}")

            num_net_interfaces = self.net_interface_count()

            if num_net_interfaces > NUM_PORTS:
                raise ValueError(f"persistent dvs is not valid for testbed with ports > {NUM_PORTS}")

            if num_net_interfaces < NUM_PORTS and not forcedvs:
                raise ValueError(f"persistent dvs does not have {NUM_PORTS} ports needed by testbed")

            # get base container
            for ctn in self.client.containers.list():
                if ctn.id == ctn_sw_id or ctn.name == ctn_sw_id:
                    ctn_sw_name = ctn.name

            if ctn_sw_name:
                _, output = subprocess.getstatusoutput(f"docker inspect --format '{{{{.State.Pid}}}}' {ctn_sw_name}")
                self.ctn_sw_pid = int(output)

                # create virtual servers
                self.servers = []
                for i in range(NUM_PORTS):
                    server = VirtualServer(ctn_sw_name, self.ctn_sw_pid, i)
                    self.servers.append(server)

                self.mount = f"/var/run/redis-vs/{ctn_sw_name}"
            else:
                self.mount = "/var/run/redis-vs/{}".format(name)

            self.net_cleanup()

            # As part of https://github.com/Azure/sonic-buildimage/pull/4499
            # VS support dynamically create Front-panel ports so save the orginal
            # config db for persistent DVS
            self.runcmd("mv /etc/sonic/config_db.json /etc/sonic/config_db.json.orig")
            self.ctn_restart()

        # Dynamically create a DVS container and servers
        else:
            self.ctn_sw = self.client.containers.run("debian:jessie",
                                                     privileged=True,
                                                     detach=True,
                                                     command="bash",
                                                     stdin_open=True)

            _, output = subprocess.getstatusoutput(f"docker inspect --format '{{{{.State.Pid}}}}' {self.ctn_sw.name}")
            self.ctn_sw_pid = int(output)

            # create virtual server
            self.servers = []
            self.create_servers()

            if self.vct:
                self.vct_connect(newctnname)

            # mount redis to base to unique directory
            self.mount = f"/var/run/redis-vs/{self.ctn_sw.name}"
            ensure_system(f"mkdir -p {self.mount}")

            kwargs = {}
            if newctnname:
                kwargs["name"] = newctnname
                self.dvsname = newctnname
            vols = {self.mount: {"bind": "/var/run/redis", "mode": "rw"}}
            if ctnmounts:
                for k, v in ctnmounts.items():
                    vols[k] = v
            kwargs["volumes"] = vols

            # create virtual switch container
            self.ctn = self.client.containers.run(imgname,
                                                  privileged=True,
                                                  detach=True,
                                                  environment=env,
                                                  network_mode=f"container:{self.ctn_sw.name}",
                                                  cpu_count=max_cpu,
                                                  **kwargs)

        _, output = subprocess.getstatusoutput(f"docker inspect --format '{{{{.State.Pid}}}}' {self.ctn.name}")

        self.pid = int(output)
        self.redis_sock = os.path.join(self.mount, "redis.sock")
        self.redis_chassis_sock = os.path.join(self.mount, "redis_chassis.sock")

        self.reset_dbs()

        # Make sure everything is up and running before turning over control to the caller
        self.check_ready_status_and_init_db()

        # Switch buffer model to dynamic if necessary
        if buffer_model == 'dynamic':
            enable_dynamic_buffer(self.get_config_db(), self.runcmd)

    def create_servers(self):
        for i in range(NUM_PORTS):
            server = VirtualServer(self.ctn_sw.name, self.ctn_sw_pid, i)
            self.servers.append(server)

    def reset_dbs(self):
        # DB wrappers are declared here, lazy-loaded in the tests
        self.app_db = None
        self.asic_db = None
        self.counters_db = None
        self.config_db = None
        self.flex_db = None
        self.state_db = None

    def del_appl_db(self):
        # APPL DB may not always exist, so use this helper method to check before deleting
        if getattr(self, 'appldb', False):
            del self.appldb


    def destroy(self) -> None:
        self.del_appl_db()

        # In case persistent dvs was used removed all the extra server link
        # that were created
        if self.persistent:
            self.destroy_servers()

        # persistent and clean-up flag are mutually exclusive
        elif self.cleanup:
            self.ctn.remove(force=True)
            self.ctn_sw.remove(force=True)
            os.system(f"rm -rf {self.mount}")
            self.destroy_servers()

    def destroy_servers(self):
        for s in self.servers:
            s.destroy()
        self.servers = []

    def check_ready_status_and_init_db(self) -> None:
        try:
            # temp fix: remove them once they are moved to vs start.sh
            self.ctn.exec_run("sysctl -w net.ipv6.conf.default.disable_ipv6=0")
            for i in range(0, 128, 4):
                self.ctn.exec_run(f"sysctl -w net.ipv6.conf.eth{i + 1}.disable_ipv6=1")

            # Verify that all of the device services have started.
            self.check_services_ready()

            # Initialize the databases.
            self.init_asic_db_validator()
            self.init_appl_db_validator()
            self.reset_dbs()

            # Verify that SWSS has finished initializing.
            self.check_swss_ready()

        except Exception:
            self.get_logs()
            self.destroy()
            raise

    def check_services_ready(self, timeout=60) -> None:
        """Check if all processes in the DVS are ready."""
        service_polling_config = PollingConfig(1, timeout, strict=True)

        def _polling_function():
            res = self.ctn.exec_run("supervisorctl status")
            out = res.output.decode("utf-8")

            process_status = {}
            for line in out.splitlines():
                tokens = line.split()

                if len(tokens) < 2:
                    continue

                process_status[tokens[0]] = tokens[1]

            for pname in self.alld:
                if process_status.get(pname, None) != "RUNNING":
                    return (False, process_status)

            return (process_status.get("start.sh", None) == "EXITED", process_status)

        wait_for_result(_polling_function, service_polling_config)

    def init_asic_db_validator(self) -> None:
        self.get_config_db()
        metadata = self.config_db.get_entry('DEVICE_METADATA|localhost', '')
        self.asicdb = AsicDbValidator(self.ASIC_DB_ID, self.redis_sock, metadata.get("switch_type"))

    def init_appl_db_validator(self) -> None:
        self.appldb = ApplDbValidator(self.APPL_DB_ID, self.redis_sock)

    def check_swss_ready(self, timeout: int = 300) -> None:
        """Verify that SWSS is ready to receive inputs.

        Almost every part of orchagent depends on ports being created and initialized
        before they can proceed with their processing. If we start the tests after orchagent
        has started running but before it has had time to initialize all the ports, then the
        first several tests will fail.
        """
        num_ports = NUM_PORTS

        # Voq and fabric asics have fabric ports enabled
        self.get_config_db()
        metadata = self.config_db.get_entry('DEVICE_METADATA|localhost', '')
        if metadata.get('switch_type', 'npu') in ['voq', 'fabric']:
            num_ports = NUM_PORTS + FABRIC_NUM_PORTS

        # Verify that all ports have been initialized and configured
        app_db = self.get_app_db()
        startup_polling_config = PollingConfig(5, timeout, strict=True)

        def _polling_function():
            port_table_keys = app_db.get_keys("PORT_TABLE")
            return ("PortInitDone" in port_table_keys and "PortConfigDone" in port_table_keys, None)

        if metadata.get('switch_type') not in ['fabric']:
            wait_for_result(_polling_function, startup_polling_config)

        # Verify that all ports have been created
        if metadata.get('switch_type') not in ['fabric']:
            asic_db = self.get_asic_db()
            asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_ports + 1)  # +1 CPU Port

        # Verify that fabric ports are monitored in STATE_DB
        if metadata.get('switch_type', 'npu') in ['voq', 'fabric']:
            self.get_state_db()
            self.state_db.wait_for_n_keys("FABRIC_PORT_TABLE", FABRIC_NUM_PORTS)

    def net_cleanup(self) -> None:
        """Clean up network, remove extra links."""
        re_space = re.compile(r'\s+')

        res = self.ctn.exec_run("ip link show")
        out = res.output.decode("utf-8")
        for line in out.splitlines():
            m = re.compile(r'^\d+').match(line)

            if not m:
                continue

            fds = re_space.split(line)
            if len(fds) > 1:
                pname = fds[1].rstrip(":")
                m = re.compile("(eth|lo|Bridge|Ethernet|vlan|inband)").match(pname)

                if not m:
                    self.ctn.exec_run(f"ip link del {pname}")
                    print(f"remove extra link {pname}")

    def net_interface_count(self) -> int:
        """Get the interface count in persistent DVS Container.

        Returns:
            The interface count, or 0 if the value is not found or some error occurs.
        """
        res = self.ctn.exec_run(["sh", "-c", "ip link show | grep -oE eth[0-9]+ | grep -vc eth0"])

        if not res.exit_code:
            out = res.output.decode("utf-8")
            return int(out.rstrip('\n'))
        else:
            return 0

    def vct_connect(self, ctnname: str) -> None:
        data = self.vct.get_inband(ctnname)

        if "inband_address" in data:
            ifpair = data["inband_intf_pair"]
            ifname = data["inband_intf"]
            iaddr = data["inband_address"]

            self.vct.connect(ifname, ifpair, str(self.ctn_sw_pid))
            self.ctn_sw.exec_run(f"ip link set dev {ifpair} up")
            self.ctn_sw.exec_run(f"ip link add link {ifpair} name vlan4094 type vlan id 4094")
            self.ctn_sw.exec_run(f"ip addr add {iaddr} dev vlan4094")
            self.ctn_sw.exec_run("ip link set dev vlan4094 up")

    def ctn_restart(self) -> None:
        self.ctn.restart()

    def restart(self) -> None:
        self.del_appl_db()

        self.ctn_restart()
        self.check_ready_status_and_init_db()

    def runcmd(self, cmd: str) -> Tuple[int, str]:
        res = self.ctn.exec_run(cmd)
        exitcode = res.exit_code
        out = res.output.decode("utf-8")

        if exitcode != 0:
            print(f"-----rc={exitcode} for cmd {cmd}-----")
            print(out.rstrip())
            print("-----")

        return (exitcode, out)

    # used in buildimage tests, do not delete
    def copy_file(self, path: str, filename: str) -> None:
        tarstr = io.BytesIO()
        tar = tarfile.open(fileobj=tarstr, mode="w")
        tar.add(filename, os.path.basename(filename))
        tar.close()

        self.ctn.exec_run(f"mkdir -p {path}")
        self.ctn.put_archive(path, tarstr.getvalue())
        tarstr.close()

    def get_logs(self) -> None:
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

    def add_log_marker(self, file_name=None) -> str:
        marker = f"=== start marker {datetime.now().isoformat()} ==="

        if file_name:
            self.runcmd(["sh", "-c", f"echo \"{marker}\" >> {file_name}"])
        else:
            self.ctn.exec_run(f"logger {marker}")

        return marker

    # start processes in SWSS
    # deps: acl, fdb, port_an, port_config, warm_reboot
    def start_swss(self):
        cmd = ""
        for pname in self.swssd:
            cmd += "supervisorctl start {}; ".format(pname)
        self.runcmd(['sh', '-c', cmd])
        time.sleep(5)

    # stop processes in SWSS
    # deps: acl, fdb, port_an, port_config, warm_reboot
    def stop_swss(self):
        cmd = ""
        for pname in self.swssd:
            cmd += "supervisorctl stop {}; ".format(pname)
        self.runcmd(['sh', '-c', cmd])
        time.sleep(5)

    def stop_syncd(self):
        self.runcmd(['sh', '-c', 'supervisorctl stop syncd'])
        time.sleep(5)

    # deps: warm_reboot
    def start_zebra(self):
        self.runcmd(['sh', '-c', 'supervisorctl start zebra'])

        # Let's give zebra a chance to connect to FPM.
        time.sleep(5)

    # deps: warm_reboot
    def stop_zebra(self):
        self.runcmd(['sh', '-c', 'pkill -9 zebra'])
        time.sleep(5)

    # deps: warm_reboot
    def start_fpmsyncd(self):
        self.runcmd(['sh', '-c', 'supervisorctl start fpmsyncd'])

        # Let's give fpmsyncd a chance to connect to Zebra.
        time.sleep(5)

    # deps: warm_reboot
    def stop_fpmsyncd(self):
        self.runcmd(['sh', '-c', 'pkill -x fpmsyncd'])
        time.sleep(1)

    # deps: warm_reboot
    def SubscribeAppDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.APPL_DB,
                        encoding="utf-8", decode_responses=True)
        pubsub = r.pubsub()
        pubsub.psubscribe("__keyspace@0__:%s*" % objpfx)
        return pubsub

    # deps: warm_reboot
    def SubscribeAsicDbObject(self, objpfx):
        r = redis.Redis(unix_socket_path=self.redis_sock, db=swsscommon.ASIC_DB,
                        encoding="utf-8", decode_responses=True)
        pubsub = r.pubsub()
        pubsub.psubscribe("__keyspace@1__:ASIC_STATE:%s*" % objpfx)
        return pubsub

    # deps: warm_reboot
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

    # deps: warm_reboot
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

    # deps: warm_reboot
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

    # deps: warm_reboot
    def SubscribeDbObjects(self, dbobjs):
        # assuming all the db object pairs are in the same db instance
        r = redis.Redis(unix_socket_path=self.redis_sock, encoding="utf-8",
                        decode_responses=True)
        pubsub = r.pubsub()
        substr = ""
        for db, obj in dbobjs:
            pubsub.psubscribe("__keyspace@{}__:{}".format(db, obj))
        return pubsub

    # deps: warm_reboot
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

    # deps: fdb_update, fdb
    def get_map_iface_bridge_port_id(self, asic_db):
        port_id_2_iface = self.asicdb.portoidmap
        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        iface_2_bridge_port_id = {}
        for key in tbl.getKeys():
            status, data = tbl.get(key)
            assert status
            values = dict(data)
            if "SAI_BRIDGE_PORT_ATTR_PORT_ID" in values:
                iface_id = values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]
                iface_name = port_id_2_iface[iface_id]
                iface_2_bridge_port_id[iface_name] = key

        return iface_2_bridge_port_id

    # deps: fdb_update, fdb
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

    # deps: fdb
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

    # deps: fdb
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

    # deps: fdb
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

    # deps: fdb_update, fdb
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

    # deps: fdb_update, fdb
    def create_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set("Vlan" + vlan, fvs)
        time.sleep(1)

    # deps: fdb_update, fdb
    def remove_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    # deps: fdb_update, fdb
    def create_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    # deps: fdb_update, fdb
    def remove_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)

    # deps: fdb
    def create_vlan_member_tagged(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "tagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    # deps: fdb_update, fdb, mirror_port_erspan, mirror_port_span, vlan
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

    # deps: acl, fdb_update, fdb, mirror_port_erspan, vlan, sub port intf
    def add_ip_address(self, interface, ip, vrf_name=None):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        pairs = [("NULL", "NULL")]
        if vrf_name:
            pairs = [("vrf_name", vrf_name)]
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(interface, fvs)
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    # deps: acl, fdb_update, fdb, mirror_port_erspan, vlan
    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip)
        tbl._del(interface)
        time.sleep(1)

    # deps: vlan
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

    # deps: acl, mirror_port_erspan
    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4")])
        tbl.set(interface + ":" + ip, fvs)
        time.sleep(1)

    # deps: acl, mirror_port_erspan
    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    # deps: mirror_port_erspan, warm_reboot
    def add_route(self, prefix, nexthop):
        self.runcmd("ip route add " + prefix + " via " + nexthop)
        time.sleep(1)

    # deps: mirror_port_erspan, warm_reboot
    def change_route(self, prefix, nexthop):
        self.runcmd("ip route change " + prefix + " via " + nexthop)
        time.sleep(1)

    # deps: warm_reboot
    def change_route_ecmp(self, prefix, nexthops):
        cmd = ""
        for nexthop in nexthops:
            cmd += " nexthop via " + nexthop

        self.runcmd("ip route change " + prefix + cmd)
        time.sleep(1)

    # deps: acl, mirror_port_erspan
    def remove_route(self, prefix):
        self.runcmd("ip route del " + prefix)
        time.sleep(1)

    # deps: mirror_port_erspan
    def create_fdb(self, vlan, mac, interface):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        fvs = swsscommon.FieldValuePairs([("port", interface),
                                          ("type", "dynamic")])
        tbl.set("Vlan" + vlan + ":" + mac, fvs)
        time.sleep(1)

    # deps: mirror_port_erspan
    def remove_fdb(self, vlan, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        tbl._del("Vlan" + vlan + ":" + mac)
        time.sleep(1)

    # deps: acl, fdb_update, fdb, intf_mac, mirror_port_erspan, mirror_port_span,
    # policer, port_dpb_vlan, vlan
    def setup_db(self):
        self.pdb = swsscommon.DBConnector(0, self.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, self.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, self.redis_sock, 0)

    def getSwitchOid(self):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        keys = tbl.getKeys()
        return str(keys[0])

    def getVlanOid(self, vlanId):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_oid = None
        keys = tbl.getKeys()
        for k in keys:
            (status, fvs) = tbl.get(k)
            assert status == True, "Could not read vlan from DB"
            for fv in fvs:
                if fv[0] == "SAI_VLAN_ATTR_VLAN_ID" and fv[1] == str(vlanId):
                    vlan_oid = str(k)
                    break
        return vlan_oid

    # deps: acl_portchannel, fdb
    def getCrmCounterValue(self, key, counter):
        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, self.redis_sock, 0)
        crm_stats_table = swsscommon.Table(counters_db, 'CRM')

        for k in crm_stats_table.get(key)[1]:
            if k[0] == counter:
                return int(k[1])

    def port_field_set(self, port, field, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([(field, value)])
        tbl.set(port, fvs)
        time.sleep(1)

    def port_admin_set(self, port, status):
        self.port_field_set(port, "admin_status", status)

    def interface_ip_add(self, port, ip_address):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(port, fvs)
        tbl.set(port + "|" + ip_address, fvs)
        time.sleep(1)

    def crm_poll_set(self, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "CRM")
        fvs = swsscommon.FieldValuePairs([("polling_interval", value)])
        tbl.set("Config", fvs)
        time.sleep(1)

    def clear_fdb(self):
        adb = swsscommon.DBConnector(0, self.redis_sock, 0)
        opdata = ["ALL", "ALL"]
        msg = json.dumps(opdata,separators=(',',':'))
        adb.publish('FLUSHFDBREQUEST', msg)

    def warm_restart_swss(self, enable):
        db = swsscommon.DBConnector(6, self.redis_sock, 0)

        tbl = swsscommon.Table(db, "WARM_RESTART_ENABLE_TABLE")
        fvs = swsscommon.FieldValuePairs([("enable",enable)])
        tbl.set("swss", fvs)

    # nat
    def nat_mode_set(self, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "NAT_GLOBAL")
        fvs = swsscommon.FieldValuePairs([("admin_mode", value)])
        tbl.set("Values", fvs)
        time.sleep(1)

    def nat_timeout_set(self, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "NAT_GLOBAL")
        fvs = swsscommon.FieldValuePairs([("nat_timeout", value)])
        tbl.set("Values", fvs)
        time.sleep(1)

    def nat_udp_timeout_set(self, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "NAT_GLOBAL")
        fvs = swsscommon.FieldValuePairs([("nat_udp_timeout", value)])
        tbl.set("Values", fvs)
        time.sleep(1)

    def nat_tcp_timeout_set(self, value):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "NAT_GLOBAL")
        fvs = swsscommon.FieldValuePairs([("nat_tcp_timeout", value)])
        tbl.set("Values", fvs)
        time.sleep(1)

    def add_nat_basic_entry(self, external, internal):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAT")
        fvs = swsscommon.FieldValuePairs([("local_ip", internal)])
        tbl.set(external, fvs)
        time.sleep(1)

    def del_nat_basic_entry(self, external):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAT")
        tbl._del(external)
        time.sleep(1)

    def add_nat_udp_entry(self, external, extport, internal, intport):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAPT")
        fvs = swsscommon.FieldValuePairs([("local_ip", internal), ("local_port", intport)])
        tbl.set(external + "|UDP|" + extport, fvs)
        time.sleep(1)

    def del_nat_udp_entry(self, external, extport):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAPT")
        tbl._del(external + "|UDP|" + extport)
        time.sleep(1)

    def add_twice_nat_basic_entry(self, external, internal, nat_type, twice_nat_id):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAT")
        fvs = swsscommon.FieldValuePairs([("local_ip", internal), ("nat_type", nat_type), ("twice_nat_id", twice_nat_id)])
        tbl.set(external, fvs)
        time.sleep(1)

    def del_twice_nat_basic_entry(self, external):
        self.del_nat_basic_entry(external)

    def add_twice_nat_udp_entry(self, external, extport, internal, intport, nat_type, twice_nat_id):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        tbl = swsscommon.Table(cdb, "STATIC_NAPT")
        fvs = swsscommon.FieldValuePairs([("local_ip", internal), ("local_port", intport), ("nat_type", nat_type), ("twice_nat_id", twice_nat_id)])
        tbl.set(external + "|UDP|" + extport, fvs)
        time.sleep(1)

    def del_twice_nat_udp_entry(self, external, extport):
        self.del_nat_udp_entry(external, extport)

    def set_nat_zone(self, interface, nat_zone):
        cdb = swsscommon.DBConnector(4, self.redis_sock, 0)
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("nat_zone", nat_zone)])
        tbl.set(interface, fvs)
        time.sleep(1)

    # deps: acl, crm, fdb
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

    # FIXME: Now that ApplDbValidator is using DVSDatabase we should converge this with
    # that implementation. Save it for a follow-up PR.
    def get_app_db(self) -> ApplDbValidator:
        if not self.app_db:
            self.app_db = DVSDatabase(self.APPL_DB_ID, self.redis_sock)

        return self.app_db

    # FIXME: Now that AsicDbValidator is using DVSDatabase we should converge this with
    # that implementation. Save it for a follow-up PR.
    def get_asic_db(self) -> AsicDbValidator:
        if not self.asic_db:
            db = DVSDatabase(self.ASIC_DB_ID, self.redis_sock)
            db.default_acl_tables = self.asicdb.default_acl_tables
            db.default_acl_entries = self.asicdb.default_acl_entries
            db.default_copp_policers = self.asicdb.default_copp_policers
            db.port_name_map = self.asicdb.portnamemap
            db.default_vlan_id = self.asicdb.default_vlan_id
            db.port_to_id_map = self.asicdb.portoidmap
            db.hostif_name_map = self.asicdb.hostifnamemap
            self.asic_db = db

        return self.asic_db

    def get_counters_db(self) -> DVSDatabase:
        if not self.counters_db:
            self.counters_db = DVSDatabase(self.COUNTERS_DB_ID, self.redis_sock)

        return self.counters_db

    def get_config_db(self) -> DVSDatabase:
        if not self.config_db:
            self.config_db = DVSDatabase(self.CONFIG_DB_ID, self.redis_sock)

        return self.config_db

    def get_flex_db(self) -> DVSDatabase:
        if not self.flex_db:
            self.flex_db = DVSDatabase(self.FLEX_COUNTER_DB_ID, self.redis_sock)

        return self.flex_db

    def get_state_db(self) -> DVSDatabase:
        if not self.state_db:
            self.state_db = DVSDatabase(self.STATE_DB_ID, self.redis_sock)

        return self.state_db

    def change_port_breakout_mode(self, intf_name, target_mode, options=""):
        cmd = f"config interface breakout {intf_name} {target_mode} -y {options}"
        self.runcmd(cmd)
        time.sleep(2)

class DockerVirtualChassisTopology:
    def __init__(
        self,
        namespace=None,
        imgname=None,
        keeptb=False,
        env=[],
        log_path=None,
        max_cpu=2,
        forcedvs=None,
        topoFile=None
    ):
        self.ns = namespace
        self.chassbr = "br4chs"
        self.keeptb = keeptb
        self.env = env
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
                netns = f"sudo ip netns exec {self.ns}"
            subprocess.check_output(f"{netns} {cmd}", stderr=subprocess.STDOUT, shell=True)
        except subprocess.CalledProcessError as e:
            print(f"------rc={e.returncode} for cmd: {e.cmd}------")
            print(e.output.rstrip())
            print("------")
            return e.returncode
        return 0

    def connect(self, ifname, ifpair, pid):
        self.runcmd(f"ip link del {ifname}")
        self.runcmd(f"ip link add {ifname} type veth peer name {ifpair}")
        self.runcmd(f"ip link set {ifpair} netns {pid}")
        self.runcmd(f"ip link set dev {ifname} up")
        self.runcmd(f"brctl addif {self.chassbr} {ifname}")

    def connect_ethintfs(self, intfs, nbrConns, pid, ctnname):
        for intf in intfs:
            ifn = f"{ctnname[:9]}.{intf}"
            self.runcmd(f"ip link add {ifn} type veth peer name {intf}")
            self.runcmd(f"ip link set {intf} netns {pid}")
            self.runcmd(f"ip link set dev {ifn} up")

        for intf in nbrConns:
            br = nbrConns[intf]
            if br != "":
                self.runcmd(f"brctl addif {br} {intf}")

    def find_all_ctns(self):
        suffix = f".{self.ns}"
        for ctn in docker.from_env().containers.list():
            if ctn.name.endswith(suffix):
                self.dvss[ctn.name] = DockerVirtualSwitch(ctn.name, self.imgname, self.keeptb,
                                                          self.env, log_path=ctn.name,
                                                          max_cpu=self.max_cpu, forcedvs=self.forcedvs,
                                                          vct=self)
        if self.chassbr is None and len(self.dvss) > 0:
            ret, res = self.ctn_runcmd(self.dvss.values()[0].ctn,
                                       "sonic-cfggen --print-data -j /usr/share/sonic/virtual_chassis/vct_connections.json")
            if ret == 0:
                out = json.loads(res)
                self.chassbr = out["chassis_bridge"]

    def get_ctn(self, ctnname):
        return self.dvss[ctnname].ctn if ctnname in self.dvss else None

    def ctn_runcmd(self, ctn, cmd):
        res = ctn.exec_run(cmd)
        exitcode = res.exit_code
        out = res.output.decode("utf-8")

        if exitcode != 0:
            print(f"-----rc={exitcode} for cmd {cmd}-----")
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
            self.runcmd(f"sudo ip netns add {self.ns}", addns=False)
            self.handle_bridge(self.chassbr)

            for ctndir in ctn:
                self.create_vct_ctn(ctndir)
            if "neighbor_connections" in self.virt_topo:
                self.handle_neighconn()
                self.handle_chassis_connections()
            retry = 0
            while self.verify_vct() is False and retry < 10:
                print("wait for chassis to be ready")
                time.sleep(1)
                retry += 1
        if self.oper == "delete":
            for dv in self.dvss.values():
                dv.destroy()
            self.handle_bridge(self.chassbr)
            self.runcmd(f"sudo ip netns del {self.ns}", addns=False)

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
            self.runcmd(f"brctl addbr {brName}")
            self.runcmd(f"ip link set dev {brName} up")
        else:
            self.runcmd(f"ip link set dev {brName} down")
            self.runcmd(f"brctl delbr {brName}")

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
                                                         env=self.env,
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
                                                                                 neighbor_address))

        return instance_to_neighbor_map

    def handle_neighconn(self):
        if self.oper != "create":
            return

        instance_to_neighbor_map = self.get_topo_neigh()
        for ctnname, nbraddrs in instance_to_neighbor_map.items():
            if ctnname not in self.dvss:
                continue

            for server, neighbor_address in nbraddrs:
                self.dvss[ctnname].servers[server].runcmd("ifconfig eth0 down")
                self.dvss[ctnname].servers[server].runcmd("ifconfig eth0 up")
                self.dvss[ctnname].servers[server].runcmd(f"ifconfig eth0 {neighbor_address}")

    def get_chassis_instance_port_statuses(self):
        instance_to_port_status_map = {}
        if "neighbor_connections" not in self.virt_topo:
            return instance_to_port_status_map

        working_dir = os.getcwd()
        for conn, endpoints in self.virt_topo["neighbor_connections"].items():
            chassis_instance = conn.split('-')[0]

            chassis_config_dir = os.path.join(working_dir, "virtual_chassis", chassis_instance)
            chassis_config_file = os.path.join(chassis_config_dir, "default_config.json")
            with open(chassis_config_file, "r") as cfg:
                config = json.load(cfg)
                device_info = config["DEVICE_METADATA"]["localhost"]
                chassis_container_name = device_info["hostname"] + "." + self.ns

                port_info = config["PORT"]

            for port, config in port_info.items():
                if "admin_status" not in config:
                    continue

                if chassis_container_name not in instance_to_port_status_map:
                    instance_to_port_status_map[chassis_container_name] = []

                instance_to_port_status_map[chassis_container_name].append((port, config.get("admin_status")))

            return instance_to_port_status_map

    def handle_chassis_connections(self):
        if self.oper != "create":
            return

        instance_to_port_status_map = self.get_chassis_instance_port_statuses()
        for chassis_instance, port_statuses in instance_to_port_status_map.items():
            if chassis_instance not in self.dvss:
                continue

            for port, status in port_statuses:
                command = "startup" if status == "up" else "shutdown"
                self.dvss[chassis_instance].runcmd(f"config interface {command} {port}")

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

@pytest.fixture(scope="session")
def manage_dvs(request) -> str:
    """
    Main fixture to manage the lifecycle of the DVS (Docker Virtual Switch) for testing

    Returns:
        (func) update_dvs function which can be called on a per-module basis
               to handle re-creating the DVS if necessary
    """
    if sys.version_info[0] < 3:
        raise NameError("Python 2 is not supported, please install python 3")

    if subprocess.check_call(["/sbin/modprobe", "team"]):
        raise NameError("Cannot install kernel team module, please install a generic kernel")

    name = request.config.getoption("--dvsname")
    using_persistent_dvs = name is not None
    forcedvs = request.config.getoption("--forcedvs")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    max_cpu = request.config.getoption("--max_cpu")
    buffer_model = request.config.getoption("--buffer_model")
    force_recreate = request.config.getoption("--force-recreate-dvs")
    graceful_stop = request.config.getoption("--graceful-stop")

    dvs = None
    curr_dvs_env = [] # lgtm[py/unused-local-variable]

    if using_persistent_dvs and force_recreate:
        pytest.fail("Options --dvsname and --force-recreate-dvs are mutually exclusive")

    def update_dvs(log_path, new_dvs_env=[]):
        """
        Decides whether or not to create a new DVS

        Create a new the DVS in the following cases:
        1. CLI option `--force-recreate-dvs` was specified (recreate for every module)
        2. The dvs_env has changed (this can only be set at container creation,
           so it is necessary to spin up a new DVS)
        3. No DVS currently exists (i.e. first time startup)

        Otherwise, restart the existing DVS (to get to a clean state)

        Returns:
            (DockerVirtualSwitch) a DVS object
        """
        nonlocal curr_dvs_env, dvs
        if force_recreate or \
           new_dvs_env != curr_dvs_env or \
           dvs is None:

            if dvs is not None:
                dvs.get_logs()
                dvs.destroy()

            dvs = DockerVirtualSwitch(name, imgname, keeptb, new_dvs_env, log_path, max_cpu, forcedvs, buffer_model = buffer_model)

            curr_dvs_env = new_dvs_env

        else:
            # First generate GCDA files for GCov
            dvs.runcmd('killall5 -15')
            # If not re-creating the DVS, restart container
            # between modules to ensure a consistent start state
            dvs.net_cleanup()
            dvs.destroy_servers()
            dvs.create_servers()
            dvs.restart()

        return dvs

    yield update_dvs

    if graceful_stop:
        dvs.stop_swss()
        dvs.stop_syncd()
    dvs.get_logs()
    dvs.destroy()

    if dvs.persistent:
        dvs.runcmd("mv /etc/sonic/config_db.json.orig /etc/sonic/config_db.json")
        dvs.ctn_restart()

@pytest.fixture(scope="module")
def dvs(request, manage_dvs) -> DockerVirtualSwitch:
    dvs_env = getattr(request.module, "DVS_ENV", [])
    name = request.config.getoption("--dvsname")
    log_path = name if name else request.module.__name__

    return manage_dvs(log_path, dvs_env)

@pytest.yield_fixture(scope="module")
def vst(request):
    vctns = request.config.getoption("--vctns")
    topo = request.config.getoption("--topo")
    forcedvs = request.config.getoption("--forcedvs")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    max_cpu = request.config.getoption("--max_cpu")
    log_path = vctns if vctns else request.module.__name__
    dvs_env = getattr(request.module, "DVS_ENV", [])
    if not topo:
        # use ecmp topology as default
        topo = "virtual_chassis/chassis_supervisor.json"
    vct = DockerVirtualChassisTopology(vctns, imgname, keeptb, dvs_env, log_path, max_cpu,
                                       forcedvs, topo)
    yield vct
    vct.get_logs(request.module.__name__)
    vct.destroy()

@pytest.fixture(scope="module")
def vct(request):
    vctns = request.config.getoption("--vctns")
    topo = request.config.getoption("--topo")
    forcedvs = request.config.getoption("--forcedvs")
    keeptb = request.config.getoption("--keeptb")
    imgname = request.config.getoption("--imgname")
    max_cpu = request.config.getoption("--max_cpu")
    log_path = vctns if vctns else request.module.__name__
    dvs_env = getattr(request.module, "DVS_ENV", [])
    if not topo:
        # use ecmp topology as default
        topo = "virtual_chassis/chassis_with_ecmp_neighbors.json"
    vct = DockerVirtualChassisTopology(vctns, imgname, keeptb, dvs_env, log_path, max_cpu,
                                       forcedvs, topo)
    yield vct
    vct.get_logs(request.module.__name__)
    vct.destroy()


@pytest.fixture
def testlog(request, dvs):
    dvs.runcmd(f"logger -t pytest === start test {request.node.nodeid} ===")
    yield testlog
    dvs.runcmd(f"logger -t pytest === finish test {request.node.nodeid} ===")

################# DVSLIB module manager fixtures #############################
@pytest.fixture(scope="class")
def dvs_acl(request, dvs) -> DVSAcl:
    return DVSAcl(dvs.get_asic_db(),
                  dvs.get_config_db(),
                  dvs.get_state_db(),
                  dvs.get_counters_db())


@pytest.fixture(scope="class")
def dvs_pbh(request, dvs) -> DVSPbh:
    return DVSPbh(dvs.get_asic_db(),
                  dvs.get_config_db())


@pytest.fixture(scope="class")
def dvs_route(request, dvs) -> DVSRoute:
    return DVSRoute(dvs.get_asic_db(),
                    dvs.get_config_db())


# FIXME: The rest of these also need to be reverted back to normal fixtures to
# appease the linter.
@pytest.fixture(scope="class")
def dvs_lag_manager(request, dvs):
    request.cls.dvs_lag = dvs_lag.DVSLag(dvs.get_asic_db(),
                                         dvs.get_config_db(),
                                         dvs)


@pytest.fixture(scope="class")
def dvs_vlan_manager(request, dvs):
    request.cls.dvs_vlan = dvs_vlan.DVSVlan(dvs.get_asic_db(),
                                            dvs.get_config_db(),
                                            dvs.get_state_db(),
                                            dvs.get_counters_db(),
                                            dvs.get_app_db())


@pytest.fixture(scope="class")
def dvs_port_manager(request, dvs):
    request.cls.dvs_port = dvs_port.DVSPort(dvs.get_asic_db(),
                                            dvs.get_config_db())


@pytest.fixture(scope="class")
def dvs_mirror_manager(request, dvs):
    request.cls.dvs_mirror = dvs_mirror.DVSMirror(dvs.get_asic_db(),
                                                  dvs.get_config_db(),
                                                  dvs.get_state_db(),
                                                  dvs.get_counters_db(),
                                                  dvs.get_app_db())


@pytest.fixture(scope="class")
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


@pytest.fixture(scope="module")
def dpb_setup_fixture(dvs):
    create_dpb_config_file(dvs)
    if dvs.vct is None:
        dvs.restart()
    else:
        dvs.vct.restart()
    yield
    remove_dpb_config_file(dvs)
