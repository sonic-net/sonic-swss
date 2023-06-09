from swsscommon import swsscommon
import conftest


def test_orchstats(dvs: conftest.DockerVirtualSwitch,):
    stats = dict(dvs.get_counters_db().get_entry("ORCH_STATS_TABLE", "PORT_TABLE"))
    assert(int(stats["SET"]) > 0)
