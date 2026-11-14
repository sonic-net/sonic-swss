"""PFC stat history as the Broadcom software PFC watchdog estimates it.

pfc_detect_broadcom.lua and pfc_restore.lua are run poll by poll against a
private redis-server started inside the VS container, with the queue and port
counters written by the test, so every poll's input is exact and nothing the
scripts publish reaches the switch's own orchagent.  The test plays orchagent's
part: a "storm" or "restore" the scripts publish changes the queue's status.
"""
import os
import time

import pytest
import redis

SCRIPT_DIR = "/usr/share/swss"
SOCK_NAME = "pfcwd_stat_history_test.sock"
COUNTERS_DB = 2
POLL_US = 100000
DETECTION_US = 400000

QUEUE = "oid:0x15000000000003"
PORT = "oid:0x1000000000001"
PRIO = 3

TOTAL = "EST_PORT_STAT_PFC_3_RX_PAUSE_DURATION_US"
RECENT = "EST_PORT_STAT_PFC_3_RECENT_PAUSE_TIME_US"
RECENT_TS = "EST_PORT_STAT_PFC_3_RECENT_PAUSE_TIMESTAMP"
RX = "SAI_PORT_STAT_PFC_3_RX_PKTS"


def pattern(*runs):
    """runs of (paused, polls) -> one boolean per poll"""
    out = []
    for paused, polls in runs:
        out += [paused] * polls
    return out


class Watchdog:
    def __init__(self, r, scripts, order):
        self.r = r
        self.scripts = [scripts[name] for name in order]
        self.cycle_us = 1000000
        self.rx = 0
        self.events = []

    def setup(self, action, restoration_us=400000, history="enable"):
        self.action = action
        self.r.flushall()
        self.r.hset("COUNTERS_QUEUE_INDEX_MAP", QUEUE, PRIO)
        self.r.hset("COUNTERS_QUEUE_PORT_MAP", QUEUE, PORT)
        self.r.hset("COUNTERS:" + QUEUE, mapping={
            "PFC_WD_DETECTION_TIME": DETECTION_US,
            "PFC_WD_RESTORATION_TIME": "" if restoration_us is None else restoration_us,
            "PFC_WD_ACTION": action,
            "PFC_STAT_HISTORY": history,
            "PFC_WD_STATUS": "operational",
            "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES": 0,
            "SAI_QUEUE_STAT_PACKETS": 0,
            "SAI_QUEUE_ATTR_PAUSE_STATUS": "false",
        })
        self.r.hset("COUNTERS:" + PORT, mapping={RX: 0, "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS": 0})
        self.pubsub = self.r.pubsub(ignore_subscribe_messages=True)
        self.pubsub.subscribe("PFC_WD_ACTION")

    def status(self):
        return self.r.hget("COUNTERS:" + QUEUE, "PFC_WD_STATUS").decode()

    def run(self, polls, mitigated_reads_unpaused=True):
        """One entry per poll: True when the partner is pausing the queue,
        "onset" when the queue reads paused before its PFC RX counter moves."""
        for poll, paused in enumerate(polls, 1):
            if paused is True:
                self.rx += 100
            # A queue the watchdog has acted on can read unpaused while PFC keeps
            # arriving; alert takes no action.
            mitigated = self.action != "alert" and self.status() == "stormed"
            reads_paused = bool(paused) and not (mitigated_reads_unpaused and mitigated)
            self.r.hset("COUNTERS:" + PORT, RX, self.rx)
            self.r.hset("COUNTERS:" + QUEUE, "SAI_QUEUE_ATTR_PAUSE_STATUS",
                        "true" if reads_paused else "false")
            self.cycle_us += POLL_US
            for script in self.scripts:
                self.r.eval(script, 1, QUEUE, COUNTERS_DB, "COUNTERS", POLL_US // 1000, 0,
                            self.cycle_us, 1000)
            self._apply_events(poll)

    def _apply_events(self, poll):
        # Messages on one channel arrive in order, so once this marker is back
        # every event the scripts published in this poll has been seen.
        self.r.publish("PFC_WD_ACTION", "poll_done")
        deadline = time.time() + 10
        while True:
            assert time.time() < deadline, "poll %d: marker never arrived" % poll
            msg = self.pubsub.get_message(timeout=1)
            if msg is None:
                continue
            if msg["data"] == b"poll_done":
                return
            event = msg["data"].decode().split('"')[3]
            self.events.append((poll, event))
            self.r.hset("COUNTERS:" + QUEUE, "PFC_WD_STATUS",
                        "stormed" if event == "storm" else "operational")
            self.r.hdel("COUNTERS:" + QUEUE, "PFC_WD_DETECTION_TIME_LEFT", "PFC_WD_RESTORATION_TIME_LEFT")

    def port_field(self, field):
        value = self.r.hget("COUNTERS:" + PORT, field)
        return None if value is None else int(value)


@pytest.fixture(scope="module")
def lua_redis(dvs):
    sock = os.path.join(dvs.mount, SOCK_NAME)
    dvs.runcmd("redis-server --port 0 --unixsocket /var/run/redis/%s --unixsocketperm 777 "
               "--save '' --appendonly no --daemonize yes" % SOCK_NAME)
    r = redis.Redis(unix_socket_path=sock, db=COUNTERS_DB)
    for _ in range(50):
        try:
            r.ping()
            break
        except redis.exceptions.ConnectionError:
            time.sleep(0.1)
    scripts = {}
    for name in ("pfc_detect_broadcom.lua", "pfc_restore.lua"):
        _, text = dvs.runcmd("cat %s/%s" % (SCRIPT_DIR, name), include_stderr=False)
        scripts[name] = text
    yield r, scripts
    r.shutdown(nosave=True)


ORDERS = [("pfc_detect_broadcom.lua", "pfc_restore.lua"),
          ("pfc_restore.lua", "pfc_detect_broadcom.lua")]


@pytest.fixture(params=ORDERS, ids=["detect_first", "restore_first"])
def wd(lua_redis, request):
    r, scripts = lua_redis
    return Watchdog(r, scripts, request.param)


class TestPfcWdStatHistoryLua:
    @pytest.mark.parametrize("unpaused", [True, False], ids=["reads_unpaused", "reads_paused"])
    def test_drop_storm_recorded_in_full(self, wd, unpaused):
        wd.setup("drop")
        wd.run(pattern((False, 3), (True, 20), (False, 20)), mitigated_reads_unpaused=unpaused)

        # detection and restoration timing are the watchdog's, unchanged
        assert wd.events == [(8, "storm"), (27, "restore")]
        assert wd.port_field(TOTAL) == 20 * POLL_US
        assert wd.port_field(RECENT) == 20 * POLL_US

    @pytest.mark.parametrize("unpaused", [True, False], ids=["reads_unpaused", "reads_paused"])
    def test_forward_storm_recorded_in_full(self, wd, unpaused):
        wd.setup("forward")
        wd.run(pattern((False, 3), (True, 20), (False, 20)), mitigated_reads_unpaused=unpaused)

        assert wd.events == [(8, "storm"), (27, "restore")]
        assert wd.port_field(TOTAL) == 20 * POLL_US
        assert wd.port_field(RECENT) == 20 * POLL_US

    def test_alert_storm_recorded_in_full(self, wd):
        wd.setup("alert")
        wd.run(pattern((False, 3), (True, 20), (False, 20)))

        assert wd.port_field(TOTAL) == 20 * POLL_US
        assert wd.port_field(RECENT) == 20 * POLL_US

    def test_storm_after_restore_opens_new_period(self, wd):
        wd.setup("drop")
        wd.run(pattern((False, 3), (True, 20), (False, 4), (True, 8), (False, 12)))

        assert wd.events[:2] == [(8, "storm"), (27, "restore")]
        assert wd.port_field(TOTAL) == 28 * POLL_US
        assert wd.port_field(RECENT) == 8 * POLL_US

    def test_pause_seen_before_rx_opens_new_period(self, wd):
        wd.setup("alert")
        wd.run(pattern((False, 3), (True, 5), (False, 5)))
        onset_start = wd.r.hget("TIMESTAMP", "PFCWD_POLL_TIMESTAMP_last")
        wd.run(pattern(("onset", 1), (True, 4), (False, 5)))

        assert wd.port_field(TOTAL) == 10 * POLL_US
        assert wd.port_field(RECENT) == 5 * POLL_US
        assert wd.r.hget("COUNTERS:" + PORT, RECENT_TS) == onset_start

    def test_queue_without_restoration_time(self, wd):
        wd.setup("drop", restoration_us=None)
        wd.run(pattern((False, 3), (True, 10), (False, 3), (True, 5), (False, 19)))

        assert wd.events == [(8, "storm")]
        assert wd.port_field(TOTAL) == 15 * POLL_US
        assert wd.port_field(RECENT) == 5 * POLL_US

    def test_history_disabled_records_nothing(self, wd):
        wd.setup("drop", history="disable")
        wd.run(pattern((False, 3), (True, 20), (False, 20)))

        assert wd.events == [(8, "storm"), (27, "restore")]
        port = wd.r.hgetall("COUNTERS:" + PORT)
        queue = wd.r.hgetall("COUNTERS:" + QUEUE)
        assert not [f for f in port if f.decode().startswith("EST_") or f.decode().endswith("_history_last")]
        assert b"PFC_STAT_HISTORY_PAUSED_last" not in queue

    def test_first_poll_only_records_baselines(self, wd):
        wd.setup("alert")
        wd.run(pattern((True, 1)))

        assert wd.port_field(TOTAL) is None
        assert wd.port_field(RX + "_history_last") == 100
        wd.run(pattern((True, 1)))
        assert wd.port_field(TOTAL) == POLL_US
        assert wd.port_field(RECENT) == POLL_US
