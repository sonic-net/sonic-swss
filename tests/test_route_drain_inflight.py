"""
Routes in flight in the yielding route drain.

orchagent takes routes from the ZMQ route channel (swss_zmq enabled, -q) with a
drain quantum of 0 (-Q 0), so the route drain stops after every step and the
other orchs run in between. A route staged by the drain holds a reference on
its next hop group until its response is handled; a group DEL that NhgOrch
parked while the group was referenced must not remove the group while such a
route uses it, even after the group's last programmed route has moved away.
"""

import time

import pytest
from swsscommon import swsscommon

from test_nhg import TestNextHopGroupBase

ORCHAGENT_SH = "/usr/bin/orchagent.sh"
ORCHAGENT_SH_BACKUP = "/usr/bin/orchagent.sh_route_drain_inflight_backup"
CONFIG_DB_JSON = "/etc/sonic/config_db.json"
CONFIG_DB_JSON_BACKUP = "/etc/sonic/config_db.json_route_drain_inflight_backup"

# Sends route operations to orchagent's ZMQ route channel in one message, so
# they all land in the same drain pass, in the order given. Arguments:
# "<op>,<prefix>,<nexthop group index>" ...
ZMQ_ROUTES = r"""
import sys
import time
from swsscommon import swsscommon

keys, ops, fvss = [], [], []
for arg in sys.argv[1:]:
    op, prefix, index = arg.split(",")
    keys.append(prefix)
    ops.append(op)
    fvss.append([("nexthop_group", index)] if op == "SET" else [])

db = swsscommon.DBConnector("APPL_DB", 0)
client = swsscommon.ZmqClient("tcp://127.0.0.1:8100")
table = swsscommon.ZmqProducerStateTable(db, "ROUTE_TABLE", client, False)
swsscommon.zmqProducerBatchedSend(table, keys, ops, fvss)
time.sleep(1)
"""


@pytest.fixture(scope="class")
def yielding_drain(dvs):
    # RouteOrch reads routes from ZMQ only when swss_zmq is enabled, which also
    # moves orchagent and syncd to southbound ZMQ. The container loads CONFIG_DB
    # from config_db.json when it starts.
    rc, _ = dvs.runcmd(["sh", "-c", f"cp {ORCHAGENT_SH} {ORCHAGENT_SH_BACKUP} && "
                        f"cp {CONFIG_DB_JSON} {CONFIG_DB_JSON_BACKUP}"])
    assert rc == 0, "Failed to back up orchagent.sh and config_db.json"
    rc, _ = dvs.runcmd(["sh", "-c", f"sonic-cfggen -j {CONFIG_DB_JSON} "
                        "-a '{\"SYSTEM_DEFAULTS\": {\"swss_zmq\": {\"status\": \"enabled\"}}}' "
                        f"--print-data > /tmp/config_db.json && mv /tmp/config_db.json {CONFIG_DB_JSON}"])
    assert rc == 0, "Failed to enable swss_zmq in config_db.json"
    rc, _ = dvs.runcmd(["sh", "-c", "sed -i 's|/usr/bin/orchagent |/usr/bin/orchagent -q tcp://127.0.0.1 -Q 0 |g' "
                        + ORCHAGENT_SH])
    assert rc == 0, "Failed to patch orchagent.sh"
    dvs.restart()

    yield

    dvs.runcmd(["sh", "-c", f"cp {ORCHAGENT_SH_BACKUP} {ORCHAGENT_SH} && "
                f"cp {CONFIG_DB_JSON_BACKUP} {CONFIG_DB_JSON}"])
    dvs.restart()


@pytest.mark.usefixtures("yielding_drain")
class TestRouteDrainInFlight(TestNextHopGroupBase):

    def wait_for(self, condition, message, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if condition():
                return
            time.sleep(0.2)
        assert condition(), message

    def send_routes(self, *operations):
        args = [f"{op},{prefix},{index}" for op, prefix, index in operations]
        rc, out = self.dvs.runcmd(["python3", "-c", ZMQ_ROUTES] + args)
        assert rc == 0, out

    def route_next_hop(self, prefix):
        route = self.get_route_id(prefix)
        if route is None:
            return None
        return self.asic_db.get_entry(self.ASIC_RT_STR, route).get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")

    def group_members(self, group_oid):
        members = 0
        for key in self.asic_db.get_keys(self.ASIC_NHGM_STR):
            fvs = self.asic_db.get_entry(self.ASIC_NHGM_STR, key)
            if fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID") == group_oid:
                members += 1
        return members

    def group_exists(self, group_oid):
        return group_oid in self.asic_db.get_keys(self.ASIC_NHG_STR)

    def test_orchagent_runs_the_yielding_drain(self, dvs, testlog):
        rc, cmdline = dvs.runcmd(["sh", "-c", "cat /proc/$(pgrep -x orchagent)/cmdline | tr '\\0' ' '"])
        assert rc == 0
        assert "-q tcp://127.0.0.1" in cmdline and "-Q 0" in cmdline, cmdline
        rc, log = dvs.runcmd(["sh", "-c", "grep 'get_route_perf_zmq_enabled' /var/log/syslog | tail -1"])
        assert "swss_zmq status: enabled" in log, "RouteOrch is not reading routes from ZMQ"

    def test_group_del_waits_for_a_staged_route(self, dvs, testlog):
        """
        Group X has one programmed route, A, so NhgOrch parks its DEL until X
        is unreferenced. One drain pass then moves A to group Y and adds route
        B on X. Once A's response is handled, X's only reference is B's staging
        pin: the DEL must still leave X, and its members, in place for B.
        """
        self.init_test(dvs, 4)
        x, y = "nhg_inflight_x", "nhg_inflight_y"
        route_a, route_b = "60.1.0.0/24", "60.2.0.0/24"

        self.nhg_ps.set(x, swsscommon.FieldValuePairs([("nexthop", "10.0.0.1,10.0.0.3"),
                                                      ("ifname", "Ethernet0,Ethernet4")]))
        self.nhg_ps.set(y, swsscommon.FieldValuePairs([("nexthop", "10.0.0.5,10.0.0.7"),
                                                      ("ifname", "Ethernet8,Ethernet12")]))
        self.asic_db.wait_for_n_keys(self.ASIC_NHG_STR, self.asic_nhgs_count + 2)

        self.send_routes(("SET", route_a, x))
        self.wait_for(lambda: self.route_next_hop(route_a) is not None, "route A was not programmed")
        x_oid = self.route_next_hop(route_a)
        assert self.group_members(x_oid) == 2

        # A uses X, so the DEL is parked until X is unreferenced.
        self.nhg_ps._del(x)
        time.sleep(2)
        assert self.group_exists(x_oid), "X was removed while route A used it"

        # One pass. The scan walks the keys in order, so A is staged, and
        # answered, before B.
        self.send_routes(("SET", route_b, x), ("SET", route_a, y))

        def a_moved():
            next_hop = self.route_next_hop(route_a)
            return next_hop is not None and next_hop != x_oid

        while not a_moved():
            assert self.group_exists(x_oid), "X was removed while route B was in flight"
            assert self.group_members(x_oid) == 2, "X lost its members while route B was in flight"
            time.sleep(0.05)

        self.wait_for(lambda: self.route_next_hop(route_b) is not None, "route B was not programmed")
        assert self.route_next_hop(route_b) == x_oid, "route B is not on X"
        assert self.group_exists(x_oid), "X was removed under route B"
        assert self.group_members(x_oid) == 2, "X lost its members under route B"
        y_oid = self.route_next_hop(route_a)
        assert self.group_exists(y_oid) and y_oid != x_oid

        # Once B leaves X, the parked DEL completes.
        self.send_routes(("DEL", route_b, ""))
        self.wait_for(lambda: not self.group_exists(x_oid), "X was not removed after its last route went")
        assert self.route_next_hop(route_a) == y_oid

        self.send_routes(("DEL", route_a, ""))
        self.wait_for(lambda: self.get_route_id(route_a) is None, "route A was not removed")
        self.nhg_ps._del(y)
        self.wait_for(lambda: not self.group_exists(y_oid), "Y was not removed")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
