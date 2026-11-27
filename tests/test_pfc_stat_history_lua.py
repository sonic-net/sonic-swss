"""PFC stat history as the hardware PFC watchdog records it.

The hardware watchdog runs no detection plugin, so pfc_stat_history.lua is the
only plugin of its PFC_WD flex counter group. It is run poll by poll against a
private redis-server started inside the VS container, with the queue and port
counters written by the test, so every poll's input is exact and nothing the
script writes reaches the switch's own orchagent.
"""
import os
import time

import pytest
import redis

SCRIPT = "/usr/share/swss/pfc_stat_history.lua"
SOCK_NAME = "pfc_stat_history_test.sock"
COUNTERS_DB = 2
POLL_MS = 100
POLL_US = POLL_MS * 1000

PORT = "oid:0x1000000000001"
PORT2 = "oid:0x1000000000002"
Q4 = "oid:0x15000000000004"
Q3 = "oid:0x15000000000003"
Q4_PORT2 = "oid:0x15000000000014"

RECENT = "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"
TOTAL = "EST_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US"
RECENT_TS = "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIMESTAMP"
RX4 = "SAI_PORT_STAT_PFC_4_RX_PKTS"
PAUSE_LAST = "SAI_QUEUE_ATTR_PAUSE_STATUS_last"


@pytest.fixture(scope="module")
def lua_redis(dvs):
    sock = os.path.join(dvs.mount, SOCK_NAME)
    dvs.runcmd("redis-server --port 0 --unixsocket /var/run/redis/%s --unixsocketperm 777 "
               "--save '' --appendonly no --daemonize yes" % SOCK_NAME)
    r = redis.Redis(unix_socket_path=sock, db=COUNTERS_DB, decode_responses=True)
    for _ in range(50):
        try:
            r.ping()
            break
        except redis.exceptions.ConnectionError:
            time.sleep(0.1)
    _, text = dvs.runcmd("cat %s" % SCRIPT, include_stderr=False)
    yield r, r.script_load(text)
    r.shutdown(nosave=True)


class History:
    """Plays syncd's part: writes the polled counters, then runs the plugin."""

    def __init__(self, r, sha):
        self.r = r
        self.sha = sha
        self.cycle_us = 1000000000
        r.flushall()
        r.hset("COUNTERS_QUEUE_INDEX_MAP", mapping={Q4: "4", Q3: "3", Q4_PORT2: "4"})
        r.hset("COUNTERS_QUEUE_PORT_MAP", mapping={Q4: PORT, Q3: PORT, Q4_PORT2: PORT2})
        # The orch lists the ports with history enabled and flags their queues
        r.hset("PFC_WD_STAT_HISTORY:PORTS", PORT, "Ethernet0")
        for queue in (Q4, Q3, Q4_PORT2):
            r.hset("COUNTERS:" + queue, mapping={"PFC_STAT_HISTORY": "enable",
                                                 "SAI_QUEUE_ATTR_PAUSE_STATUS": "false"})
        for port in (PORT, PORT2):
            r.hset("COUNTERS:" + port, mapping={RX4: "0", "SAI_PORT_STAT_PFC_3_RX_PKTS": "0"})

    def poll(self, rx=None, paused=None, step_us=POLL_US, cycle_stamp=True):
        if rx is not None:
            for port in (PORT, PORT2):
                self.r.hset("COUNTERS:" + port, RX4, str(rx))
        if paused is not None:
            for queue in (Q4, Q4_PORT2):
                self.r.hset("COUNTERS:" + queue, "SAI_QUEUE_ATTR_PAUSE_STATUS",
                            "true" if paused else "false")
        self.cycle_us += step_us
        argv = [COUNTERS_DB, "COUNTERS", POLL_MS, 0]
        if cycle_stamp:
            argv += [self.cycle_us, 1000]
        self.r.evalsha(self.sha, 3, Q4, Q3, Q4_PORT2, *argv)

    def field(self, name, key=PORT):
        return self.r.hget("COUNTERS:" + key, name)

    def poll_timestamp(self):
        return self.r.hget("TIMESTAMP", "PFCWD_POLL_TIMESTAMP_last")


@pytest.fixture
def hist(lua_redis):
    r, sha = lua_redis
    return History(r, sha)


class TestPfcStatHistoryLua:
    def test_first_poll_only_records_baselines(self, hist):
        hist.poll(rx=0, paused=False)

        assert hist.field(RECENT) is None
        assert hist.field(TOTAL) is None
        assert hist.field(RX4 + "_last") == "0"
        assert hist.field(PAUSE_LAST, Q4) == "false"

    def test_first_poll_opens_nothing_even_with_pfc_counted(self, hist):
        hist.poll(rx=100, paused=True)

        assert hist.field(RECENT) is None
        assert hist.field(TOTAL) is None
        assert hist.field(PAUSE_LAST, Q4) == "false"

    def test_pause_at_the_baseline_poll_opens_a_new_period(self, hist):
        # history left by an earlier enable
        hist.r.hset("COUNTERS:" + PORT, mapping={RECENT: "3000000", RECENT_TS: "1700000000000000",
                                                 TOTAL: "3000000"})
        hist.poll(rx=100, paused=True)
        period_start = hist.poll_timestamp()
        hist.poll(rx=110, paused=True)

        assert hist.field(RECENT_TS) == period_start
        assert hist.field(RECENT) == str(POLL_US)
        assert hist.field(TOTAL) == str(3000000 + POLL_US)

    def test_rx_stepping_back_keeps_the_period(self, hist):
        hist.poll(rx=0, paused=False)
        period_start = hist.poll_timestamp()
        hist.poll(rx=10, paused=False)
        hist.poll(rx=20, paused=False)
        # an older read of the same counter lands after a newer one
        hist.poll(rx=15, paused=False)
        hist.poll(rx=30, paused=False)

        assert hist.field(RECENT_TS) == period_start
        assert hist.field(RECENT) == str(4 * POLL_US)
        assert hist.field(TOTAL) == str(4 * POLL_US)

    def test_rx_stepping_back_while_quiet_stays_quiet(self, hist):
        hist.poll(rx=20, paused=False)
        hist.poll(rx=20, paused=False)
        hist.poll(rx=15, paused=False)

        assert hist.field(RECENT) is None
        assert hist.field(TOTAL) is None

    def test_idle_queue_records_nothing(self, hist):
        hist.poll(rx=0, paused=False)
        hist.poll(rx=0, paused=False)

        assert hist.field(RECENT) is None
        assert hist.field(TOTAL) is None

    def test_pfc_rx_opens_a_period_and_charges_the_cycle_gap(self, hist):
        hist.poll(rx=0, paused=False)
        period_start = hist.poll_timestamp()
        hist.poll(rx=10, paused=False, step_us=150000)

        assert hist.field(RECENT_TS) == period_start
        assert hist.field(RECENT) == "150000"
        assert hist.field(TOTAL) == "150000"
        assert hist.field(PAUSE_LAST, Q4) == "true"

    def test_pause_without_new_frames_and_release(self, hist):
        hist.poll(rx=0, paused=False)
        hist.poll(rx=10, paused=False)
        hist.poll(rx=10, paused=True)
        assert hist.field(RECENT) == str(2 * POLL_US)

        hist.poll(rx=10, paused=False)
        assert hist.field(RECENT) == str(2 * POLL_US)
        assert hist.field(PAUSE_LAST, Q4) == "false"

    def test_queue_reading_unpaused_while_pfc_arrives_stays_in_the_period(self, hist):
        # A queue the watchdog has acted on can read unpaused while PFC keeps arriving
        hist.poll(rx=0, paused=False)
        hist.poll(rx=10, paused=True)
        period_start = hist.field(RECENT_TS)
        hist.poll(rx=20, paused=False)
        hist.poll(rx=30, paused=False)

        assert hist.field(RECENT_TS) == period_start
        assert hist.field(RECENT) == str(3 * POLL_US)

    def test_pause_seen_before_rx_opens_a_new_period(self, hist):
        hist.poll(rx=0, paused=False)
        hist.poll(rx=10, paused=True)
        hist.poll(rx=10, paused=False)
        period_start = hist.poll_timestamp()
        hist.poll(rx=10, paused=True)

        assert hist.field(RECENT_TS) == period_start
        assert hist.field(RECENT) == str(POLL_US)
        assert hist.field(TOTAL) == str(2 * POLL_US)

    def test_unlisted_port_and_idle_priority_untouched(self, hist):
        hist.poll(rx=0, paused=False)
        hist.poll(rx=10, paused=True)

        assert hist.field(RECENT, PORT2) is None
        assert hist.field(RX4 + "_last", PORT2) is None
        assert hist.field(PAUSE_LAST, Q4_PORT2) is None
        assert hist.field("EST_PORT_STAT_PFC_3_RECENT_PAUSE_TIME_US") is None

    def test_sai_total_pause_time_is_not_estimated(self, hist):
        hist.r.hset("COUNTERS:" + PORT, "SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US", "777")
        hist.poll(rx=0, paused=False)
        hist.poll(rx=5, paused=True)

        assert hist.field(TOTAL) is None
        assert hist.field("SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US") == "777"
        assert hist.field(RECENT) == str(POLL_US)

    def test_no_port_listed_reads_and_writes_nothing(self, hist):
        hist.r.delete("PFC_WD_STAT_HISTORY:PORTS")
        hist.poll(rx=0, paused=False)
        hist.poll(rx=5, paused=True)

        assert hist.field(RX4 + "_last") is None
        assert hist.field(RECENT) is None
        assert hist.poll_timestamp() is None

    def test_cleared_baselines_restart_from_a_baseline_poll(self, hist):
        hist.poll(rx=0, paused=False)
        hist.poll(rx=5, paused=True)
        recorded = hist.field(RECENT)
        # What the orch removes when the watchdog stops on the port
        hist.r.hdel("COUNTERS:" + Q4, PAUSE_LAST)
        hist.r.hdel("COUNTERS:" + PORT, RX4 + "_last")
        hist.poll(rx=50, paused=True)

        assert hist.field(RECENT) == recorded

    def test_realtime_gap_without_cycle_stamp(self, hist):
        hist.poll(rx=0, paused=False, cycle_stamp=False)
        time.sleep(0.05)
        hist.poll(rx=5, paused=True, cycle_stamp=False)

        assert 40000 <= int(hist.field(RECENT)) < 1000000
